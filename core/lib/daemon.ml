type start_helper = bottle:string -> (Helper_api.helper, string) result

let ( let* ) = Result.bind
let sprintf = Printf.sprintf

(* ---- home: core.sock, core.lock, events.jsonl, core.log, helper.log ---- *)

(* macOS keeps 104 bytes for a socket path, the terminating nul included *)
let max_socket_path = 103
let in_home home name = Filename.concat home name

let socket_path ~home =
  let p = in_home home "core.sock" in
  if String.length p > max_socket_path then Error (sprintf "the path %s is too long for a unix socket. set MOTH_HOME to a shorter directory." p)
  else Ok p

let make_home home =
  let rec parents d =
    if not (Sys.file_exists d) then begin
      parents (Filename.dirname d);
      try Unix.mkdir d 0o755 with Unix.Unix_error (Unix.EEXIST, _, _) -> ()
    end
  in
  if not (Sys.file_exists home) then
    try
      parents (Filename.dirname home);
      (try Unix.mkdir home 0o700 with Unix.Unix_error (Unix.EEXIST, _, _) -> ());
      Unix.chmod home 0o700
    with Unix.Unix_error (e, _, _) -> failwith (sprintf "couldn't create %s (%s). set MOTH_HOME to a directory you can write to." home (Unix.error_message e))

(* held for the daemon's whole life, so two daemons never share a home *)
let take_lock home =
  let fd = Unix.openfile (in_home home "core.lock") [ Unix.O_RDWR; Unix.O_CREAT; Unix.O_CLOEXEC ] 0o600 in
  match Unix.lockf fd Unix.F_TLOCK 0 with
  | () -> Some fd
  | exception Unix.Unix_error _ ->
      Unix.close fd;
      None

let clear_stale_socket ~home =
  match take_lock home with
  | None | (exception Unix.Unix_error _) -> false
  | Some fd ->
      (try Unix.unlink (in_home home "core.sock") with Unix.Unix_error _ -> ());
      Unix.close fd;
      true

let answers path =
  let s = Unix.socket ~cloexec:true Unix.PF_UNIX Unix.SOCK_STREAM 0 in
  Fun.protect
    ~finally:(fun () -> Unix.close s)
    (fun () -> match Unix.connect s (Unix.ADDR_UNIX path) with () -> true | exception Unix.Unix_error _ -> false)

let local_time t =
  let tm = Unix.localtime t in
  sprintf "%04d-%02d-%02d %02d:%02d:%02d" (tm.tm_year + 1900) (tm.tm_mon + 1) tm.tm_mday tm.tm_hour tm.tm_min tm.tm_sec

(* a line for core.log *)
let say fmt = Printf.ksprintf (fun s -> Printf.eprintf "%s %s\n%!" (local_time (Unix.time ())) s) fmt

(* ---- state ---- *)

type target = { proc : Helper_api.attached; bottle : string; session : Session.t }

type state = {
  home : string;
  sock : string;
  start_helper : start_helper;
  budget_mb : int option; (* from MOTH_BUDGET_MB, for every session *)
  mutable conn : (string * Helper_api.helper) option; (* the helper and the bottle it runs in *)
  mutable target : target option; (* always in the helper's bottle *)
  mutable lost : bool; (* the helper said helper_lost during this request *)
  mutable gone : bool; (* the helper said target_gone during this request *)
  mutable seq : int; (* the newest event's number *)
  mutable wrote : Yojson.Safe.t option; (* the write this request made, for its event *)
  mutable held : Yojson.Safe.t option; (* whether this request's set held, for its event *)
  mutable owned : (Unix.file_descr * Unix.file_descr) option; (* listener and lock, while serving *)
}

let not_attached = "not attached. run: moth attach --bottle <bottle> <game.exe>"
let already_running = "moth is already running"

let message (e : Helper_api.err) =
  match e.code with
  | "helper_lost" -> "lost contact with moth's helper. try again."
  | "target_gone" -> "the game closed. attach again once it's running."
  | _ -> if e.message = "" then e.code else e.message

let lift r = Result.map_error message r

(* the helper as the daemon hands it out: an error that ends the helper or the target is noted,
   so the daemon lets go of them too once the request is done *)
let watch st (h : Helper_api.helper) : Helper_api.helper =
  let see r =
    (match r with
    | Error { Helper_api.code = "helper_lost"; _ } -> st.lost <- true
    | Error { Helper_api.code = "target_gone"; _ } -> st.gone <- true
    | _ -> ());
    r
  in
  {
    ps = (fun () -> see (h.ps ()));
    modules = (fun pid -> see (h.modules pid));
    attach = (fun pid -> see (h.attach pid));
    scan = (fun ~keep ~budget_mb specs -> see (h.scan ~keep ~budget_mb specs));
    snapshot = (fun ~cap_mb ~budget_mb groups -> see (h.snapshot ~cap_mb ~budget_mb groups));
    narrow = (fun ~atomic items -> see (h.narrow ~atomic items));
    drop = (fun ids -> see (h.drop ids));
    peek = (fun ~id ~vtype ~offset ~limit -> see (h.peek ~id ~vtype ~offset ~limit));
    peek_from = (fun ~id ~vtype ~addr ~limit -> see (h.peek_from ~id ~vtype ~addr ~limit));
    read = (fun ~addr ~vtype -> see (h.read ~addr ~vtype));
    write = (fun ~addr ~vtype v -> see (h.write ~addr ~vtype v));
    status = (fun () -> see (h.status ()));
    detach = (fun () -> see (h.detach ()));
    close = h.close;
  }

(* closing the connection makes the helper exit *)
let close_helper st =
  Option.iter (fun (_, (h : Helper_api.helper)) -> h.close ()) st.conn;
  st.conn <- None;
  st.target <- None

let settle st =
  if st.lost then close_helper st else if st.gone then st.target <- None;
  st.lost <- false;
  st.gone <- false

(* lets go of the helper, then of home: the socket, the listener and the lock *)
let shutdown st =
  close_helper st;
  match st.owned with
  | None -> ()
  | Some (listener, lock) ->
      st.owned <- None;
      (try Unix.unlink st.sock with Unix.Unix_error _ -> ());
      Unix.close listener;
      Unix.close lock

(* the helper running in [bottle], started when there is none *)
let open_helper st bottle =
  match st.conn with
  | Some (b, h) when b = bottle -> Ok h
  | _ ->
      close_helper st;
      let* h = st.start_helper ~bottle in
      let h = watch st h in
      st.conn <- Some (bottle, h);
      Ok h

let processes st bottle =
  match (st.conn, st.target) with
  | Some (b, h), _ when b = bottle -> lift (h.ps ())
  | _, Some _ ->
      (* attached in another bottle: a helper just for this listing, so the hunt goes on *)
      let* h = st.start_helper ~bottle in
      Fun.protect ~finally:h.close (fun () -> lift (h.ps ()))
  | _ ->
      let* h = open_helper st bottle in
      lift (h.ps ())

let is_digits s = s <> "" && String.for_all (fun c -> c >= '0' && c <= '9') s

let pick ~bottle what procs =
  match int_of_string_opt what with
  | Some pid when is_digits what -> if List.mem_assoc pid procs then Ok pid else Error (sprintf "pid %d isn't running in %s." pid bottle)
  | _ -> (
      let wanted = String.lowercase_ascii what in
      match List.filter (fun (_, name) -> String.lowercase_ascii name = wanted) procs with
      | [] -> Error (sprintf "%s isn't running in %s. start the game first." what bottle)
      | [ (pid, _) ] -> Ok pid
      | (_, name) :: _ as many ->
          let pids = List.sort compare (List.map fst many) in
          Error (sprintf "more than one %s is running (pids %s). attach by pid." name (String.concat ", " (List.map string_of_int pids))))

(* the anti-cheat named in [found], if any: the helper is closed and attaching refused *)
let refuse_anticheat st found =
  match found with
  | Some name ->
      close_helper st;
      Error (sprintf "moth won't attach: %s is running with this game. moth is for single-player, offline play." name)
  | None -> Ok ()

(* Process names are checked before anything of the game's is opened; then its modules, and a
   module list that cannot be read refuses the attach rather than skipping the check. *)
let attach st ~bottle what =
  let* h = open_helper st bottle in
  let* procs = lift (h.ps ()) in
  let* pid = pick ~bottle what procs in
  let processes = List.map snd procs in
  let* () = refuse_anticheat st (Anticheat.check ~processes ~modules:[]) in
  let* modules =
    match h.modules pid with
    | Ok m -> Ok m
    | Error { code = "modules_failed"; _ } -> Error (sprintf "couldn't check what's loaded in %s, so moth won't attach. try again." (List.assoc pid procs))
    | Error e -> Error (message e)
  in
  let* () = refuse_anticheat st (Anticheat.check ~processes ~modules) in
  (* attaching again to the game already attached keeps its hunt, once the helper confirms it
     is still attached to that pid and the process is running; a game that has closed (whose pid
     a new one may have) is attached afresh *)
  let same t = t.proc.pid = pid && match h.status () with Ok (Some a) -> a.pid = pid | _ -> false in
  match st.target with
  | Some t when same t -> Ok t
  | _ ->
      (* the helper lets go of the old target when it attaches to the new one *)
      st.target <- None;
      let* proc = lift (h.attach pid) in
      let t = { proc; bottle; session = Session.create ?budget_mb:st.budget_mb h } in
      st.gone <- false;
      st.target <- Some t;
      Ok t

let attached_line t = sprintf "attached to %s (pid %d, %d-bit) in %s" t.proc.name t.proc.pid t.proc.bits t.bottle

let detach st =
  let text = match st.target with Some t -> "detached from " ^ t.proc.name | None -> "not attached" in
  close_helper st;
  text

(* ---- json and text for each reply ---- *)

let group_json ((g : Helper_api.group_info), tier) =
  `Assoc
    [
      ("id", `String g.id);
      ("count", `Int g.count);
      ("tracked", `Bool g.tracked);
      ("has_values", `Bool g.has_values);
      ("tier", `String (match tier with Interp.Main -> "main" | Interp.Wide -> "wide"));
    ]

let note_name = function
  | Session.Fell_back r -> "fell_back_" ^ (match r with Down -> "down" | Up -> "up" | Same -> "same" | Changed -> "changed")
  | Showing_wide -> "showing_wide"
  | Untracked -> "untracked"
  | Lost _ -> "lost"
  | Snapshot_taken -> "snapshot"

let outcome (o : Session.outcome) =
  let lost = List.fold_left (fun a -> function Session.Lost n -> a + n | _ -> a) 0 o.notes in
  ( `Assoc
      [
        ("main", `Int o.main);
        ("wide", `Int o.wide);
        ("by_tag", `List (List.map (fun (tag, c) -> `List [ `String tag; `Int c ]) o.by_tag));
        ("notes", `List (List.map (fun n -> `String (note_name n)) o.notes));
        ("lost", `Int lost);
        ("dead_end", `Bool o.dead_end);
        ("ms", `Float o.ms);
        ("bytes", `Int o.bytes);
        ("unreadable", `Int o.unreadable);
        ("groups", `List (List.map group_json o.groups));
      ],
    Render.outcome o )

let hex a = `String (sprintf "0x%Lx" a)

let listing ?near ~all (l : Session.listing) =
  let row (r : Session.row) =
    `Assoc
      [
        ("n", `Int r.n);
        ("addr", hex r.addr);
        ("interp_id", `String r.interp_id);
        ("tag", `String r.tag);
        ("shown", `String r.shown);
        ("away", match r.away with Some d -> `Int d | None -> `Null);
      ]
  in
  (`Assoc [ ("rows", `List (List.map row l.rows)); ("total", `Int l.total); ("from", `Int l.from) ], Render.listing ?near ~all l)

let kept ks =
  ( `List (List.map (fun (k : Session.kept) -> `Assoc [ ("name", `String k.name); ("addr", hex k.kaddr); ("tag", `String k.ktag); ("shown", `String k.kshown) ]) ks),
    Render.kept ks )

let said m = (`Assoc [ ("message", `String m) ], m)

(* a write for the event log: where, as which type, the bytes it replaced and the ones it put there *)
let written_json (w : Session.written) =
  `Assoc
    [
      ("addr", hex w.where);
      ("type", `String w.wtype);
      ("prev_bits", Wire.bits_to_json ~vtype:w.wtype w.bits_before);
      ("new_bits", Wire.bits_to_json ~vtype:w.wtype w.bits_after);
    ]

(* a set: what to say and whether the value held (null: it couldn't be read back); both go in
   the request's event *)
let set_done st (d : Session.set_done) =
  let held = match d.held with Some b -> `Bool b | None -> `Null in
  st.wrote <- Some (written_json d.w);
  st.held <- Some held;
  (`Assoc [ ("message", `String d.text); ("held", held) ], d.text)

(* Asks the helper whether the game is still there. When it has exited, the target goes, and its
   hunt, kept values and writes with it; the helper stays, and lets go of the game's candidates
   now rather than at the next attach (if it can't, the answer is the same). *)
let status st =
  match (st.target, st.conn) with
  | Some t, Some (_, h) -> (
      match h.status () with
      | Ok None ->
          st.target <- None;
          ignore (h.detach ());
          Ok (`Assoc [ ("attached", `Null); ("hunt", `Null); ("closed", `String t.proc.name) ], sprintf "not attached. %s closed." t.proc.name)
      | Error { code = "helper_lost"; _ } -> Error "lost contact with moth's helper. attach again."
      | Error e -> Error (message e)
      | Ok (Some _) -> (
          let attached = `Assoc [ ("pid", `Int t.proc.pid); ("name", `String t.proc.name); ("bits", `Int t.proc.bits); ("bottle", `String t.bottle) ] in
          match Session.current t.session with
          | None -> Ok (`Assoc [ ("attached", attached); ("hunt", `Null) ], attached_line t)
          | Some o ->
              let hunt = `Assoc [ ("main", `Int o.main); ("wide", `Int o.wide); ("groups", `List (List.map group_json o.groups)) ] in
              Ok (`Assoc [ ("attached", attached); ("hunt", hunt) ], attached_line t ^ "\n" ^ Render.counts o)))
  | _ -> Ok (`Assoc [ ("attached", `Null); ("hunt", `Null) ], "not attached")

let bottles () =
  match Crossover.list_bottles () with
  | [] -> (`List [], sprintf "no bottles in %s" (Crossover.bottles_dir ()))
  | names -> (`List (List.map (fun n -> `String n) names), String.concat "\n" names)

let ps_listing bottle procs =
  let json = `List (List.map (fun (pid, name) -> `Assoc [ ("pid", `Int pid); ("name", `String name) ]) procs) in
  match procs with
  | [] -> (json, sprintf "nothing is running in %s." bottle)
  | _ ->
      let w = List.fold_left (fun w (pid, _) -> max w (String.length (string_of_int pid))) 0 procs in
      (json, String.concat "\n" (List.map (fun (pid, name) -> sprintf "%*d  %s" w pid name) procs))

(* ---- the event log ---- *)

let event_line ev =
  let open Yojson.Safe.Util in
  try
    let words = to_string (member "cmd" ev) :: List.map to_string (to_list (member "args" ev)) in
    Some
      (sprintf "%d  %s  %s  %s  %s%s" (to_int (member "seq" ev))
         (local_time (to_number (member "ts" ev)))
         (to_string (member "actor" ev)) (String.concat " " words)
         (if to_bool (member "ok" ev) then "" else "error: ")
         (to_string (member "summary" ev)))
  with Type_error _ -> None

(* newest first, leaving out lines that are not events *)
let read_events home =
  match open_in_bin (in_home home "events.jsonl") with
  | exception Sys_error _ -> []
  | ic ->
      let rec go acc =
        match input_line ic with
        | exception End_of_file -> acc
        | line -> (
            match Yojson.Safe.from_string line with
            | ev when event_line ev <> None -> go (ev :: acc)
            | _ | (exception Yojson.Json_error _) -> go acc)
      in
      Fun.protect ~finally:(fun () -> close_in ic) (fun () -> go [])

let last_seq home = match read_events home with ev :: _ -> Yojson.Safe.Util.(to_int (member "seq" ev)) | [] -> 0
let first_line s = match String.index_opt s '\n' with Some i -> String.sub s 0 i | None -> s

(* one line of events.jsonl: the command, its arguments and its options (when any were given),
   how it went, and for a write, whether it held and what it wrote *)
let record st ~actor ~cmd ~args ~opts result =
  st.seq <- st.seq + 1;
  let ok, summary = match result with Ok (_, text) -> (true, first_line text) | Error e -> (false, first_line e) in
  let ev =
    `Assoc
      ([
         ("seq", `Int st.seq);
         ("ts", `Float (Float.round (Unix.gettimeofday () *. 1000.) /. 1000.));
         ("actor", `String actor);
         ("cmd", `String cmd);
         ("args", `List (List.map (fun a -> `String a) args));
       ]
      @ (if opts = [] then [] else [ ("opts", `Assoc (List.map (fun (k, v) -> (k, `String v)) opts)) ])
      @ [ ("ok", `Bool ok); ("summary", `String summary) ]
      @ (match st.held with Some h -> [ ("held", h) ] | None -> [])
      @ match st.wrote with Some w -> [ ("write", w) ] | None -> [])
  in
  try
    let oc = open_out_gen [ Open_wronly; Open_creat; Open_append; Open_binary ] 0o600 (in_home st.home "events.jsonl") in
    Fun.protect ~finally:(fun () -> close_out_noerr oc) (fun () -> output_string oc (Yojson.Safe.to_string ev ^ "\n"); close_out oc)
  with Sys_error m -> say "couldn't write the event log: %s" m

let log st opts =
  let n = match List.assoc_opt "n" opts with Some s -> int_of_string s | None -> 20 in
  let evs = List.filteri (fun i _ -> i < n) (read_events st.home) in
  (`List evs, if evs = [] then "nothing logged yet" else String.concat "\n" (List.filter_map event_line evs))

(* ---- commands ---- *)

(* a result's number, #n or n, from 1 *)
let row_number s =
  let s = if String.starts_with ~prefix:"#" s then String.sub s 1 (String.length s - 1) else s in
  match int_of_string_opt s with Some n when is_digits s && n > 0 -> Some n | _ -> None

(* a 0x address, any case *)
let address s =
  let n = String.length s in
  let hex_digit = function '0' .. '9' | 'a' .. 'f' | 'A' .. 'F' -> true | _ -> false in
  if n > 2 && s.[0] = '0' && (s.[1] = 'x' || s.[1] = 'X') && String.for_all hex_digit (String.sub s 2 (n - 2)) then
    Int64.of_string_opt ("0x" ^ String.sub s 2 (n - 2))
  else None

let check_usage ~cmd ~args ~opts =
  let need ok msg = if ok then Ok () else Error msg in
  let none = need (args = []) (cmd ^ " takes no arguments") in
  (* the option, when given, is a whole number that [ok] accepts *)
  let whole k ok = match List.assoc_opt k opts with None -> true | Some v -> is_digits v && Option.fold ~none:false ~some:ok (int_of_string_opt v) in
  match cmd with
  | "bottles" | "status" | "kept" | "undo" | "detach" | "stop" -> none
  | "ps" ->
      let* () = need (List.mem_assoc "bottle" opts) "ps needs a bottle: moth ps --bottle <bottle>" in
      none
  | "attach" -> need (List.length args = 1 && List.mem_assoc "bottle" opts) "attach needs a bottle and a process: moth attach --bottle <bottle> <game.exe>"
  | "find" -> need (List.length args = 1) "find needs a number"
  | "next" -> need (List.length args = 1) "next needs a number, or one of: down, up, same, changed"
  | "results" ->
      let* () = none in
      let* () = need (whole "n" (fun n -> n >= 1 && n <= 200)) "results -n needs a number from 1 to 200" in
      need (whole "from" (fun n -> n >= 1)) "results --from needs a row number, 1 or more"
  | "keep" ->
      (* a result's number, #3 or 3 as set takes it (a shell reads # as a comment), or an address
         and the number it shows *)
      need
        (match args with
        | ([ r ] | [ r; _ ]) when row_number r <> None -> true
        | [ a; _ ] | [ a; _; _ ] -> address a <> None
        | _ -> false)
        "keep needs a result, or an address and the number it shows: moth keep <#n> [name], or moth keep <0xaddr> <number> [name]"
  | "forget" -> need (List.length args = 1) "forget needs a name: moth forget <name>"
  | "set" ->
      (* a result's number, or else a kept value's name, which the session checks *)
      let named s = not (String.starts_with ~prefix:"#" s || is_digits s) in
      need
        (match args with [ target; _ ] -> row_number target <> None || named target | _ -> false)
        "set needs a result number or a kept name, and a number: moth set <#n|name> <number>"
  | "log" ->
      let* () = none in
      need (whole "n" (fun _ -> true)) "log -n needs a whole number"
  | _ -> Error (sprintf "unknown command: %s. run: moth help" cmd)

let relations = [ ("down", Session.Down); ("up", Session.Up); ("same", Session.Same); ("changed", Session.Changed) ]

let handle st ~cmd ~args ~opts =
  let* () = check_usage ~cmd ~args ~opts in
  let bottle () = List.assoc "bottle" opts in
  let session () = match st.target with Some t -> Ok t.session | None -> Error not_attached in
  match (cmd, args) with
  | "bottles", _ -> Ok (bottles ())
  | "ps", _ ->
      let* procs = processes st (bottle ()) in
      Ok (ps_listing (bottle ()) procs)
  | "attach", [ what ] ->
      let* t = attach st ~bottle:(bottle ()) what in
      Ok (said (attached_line t))
  | "status", _ -> status st
  | "find", [ n ] ->
      let* s = session () in
      Result.map outcome (Session.find s n)
  | "next", [ a ] ->
      let* s = session () in
      Result.map outcome
        (match List.assoc_opt (String.lowercase_ascii a) relations with Some r -> Session.next_relation s r | None -> Session.next_number s a)
  | "results", _ ->
      let* s = session () in
      let all = List.assoc_opt "all" opts = Some "true" and near = List.assoc_opt "near" opts in
      let given k default = Option.fold ~none:default ~some:int_of_string (List.assoc_opt k opts) in
      Result.map (listing ?near ~all) (Session.results s ~all ~from:(given "from" 1) ~limit:(given "n" 20) ~near)
  | "keep", first :: rest ->
      let* s = session () in
      let name = function [ n ] -> Some n | _ -> None in
      Result.map said
        (match (address first, rest) with
        | Some addr, number :: more -> Session.keep_at s ~addr number ~name:(name more)
        | _ -> Session.keep_row s ~row:(Option.get (row_number first)) ~name:(name rest))
  | "kept", _ ->
      let* s = session () in
      Result.map kept (Session.kept s)
  | "forget", [ name ] ->
      let* s = session () in
      Result.map said (Session.forget s name)
  | "set", [ target; v ] ->
      let* s = session () in
      let* d = match row_number target with Some row -> Session.set s ~row v | None -> Session.set_kept s ~name:target v in
      Ok (set_done st d)
  | "undo", _ ->
      let* s = session () in
      let* text, w = Session.undo s in
      st.wrote <- Some (written_json w);
      Ok (said text)
  | "detach", _ -> Ok (said (detach st))
  | "log", _ -> Ok (log st opts)
  | "stop", _ ->
      shutdown st;
      Ok (said "stopped")
  | _ -> Error (sprintf "unknown command: %s. run: moth help" cmd)

(* ---- the protocol ---- *)

type request = { rid : Yojson.Safe.t; actor : string; cmd : string; args : string list; opts : (string * string) list }

let parse_request line =
  match Yojson.Safe.from_string line with
  | exception Yojson.Json_error _ -> Error (`Null, "bad request: not json")
  | `Assoc f -> (
      let rid = Option.value (List.assoc_opt "id" f) ~default:`Null in
      let text = function `String s -> Some s | `Int n -> Some (string_of_int n) | `Bool b -> Some (string_of_bool b) | _ -> None in
      match List.assoc_opt "cmd" f with
      | Some (`String cmd) ->
          let actor = match List.assoc_opt "actor" f with Some (`String a) when a <> "" -> a | _ -> "user" in
          let args = match List.assoc_opt "args" f with Some (`List l) -> List.filter_map text l | _ -> [] in
          let opts = match List.assoc_opt "opts" f with Some (`Assoc l) -> List.filter_map (fun (k, v) -> Option.map (fun s -> (k, s)) (text v)) l | _ -> [] in
          Ok { rid; actor; cmd; args; opts }
      | _ -> Error (rid, "bad request: no cmd"))
  | _ -> Error (`Null, "bad request: not an object")

let reply rid = function
  | Ok (result, text) -> `Assoc [ ("id", rid); ("ok", `Bool true); ("result", result); ("text", `String text) ]
  | Error e -> `Assoc [ ("id", rid); ("ok", `Bool false); ("error", `String e) ]

let answer st line =
  match parse_request line with
  | Error (rid, msg) -> reply rid (Error msg)
  | Ok r ->
      let result =
        try handle st ~cmd:r.cmd ~args:r.args ~opts:r.opts
        with e ->
          let e = Printexc.to_string e in
          say "internal error in %s: %s" r.cmd e;
          Error (sprintf "moth hit an internal error: %s. see %s" e (in_home st.home "core.log"))
      in
      settle st;
      record st ~actor:r.actor ~cmd:r.cmd ~args:r.args ~opts:r.opts result;
      st.wrote <- None;
      st.held <- None;
      reply r.rid result

let max_request = 1 lsl 20

let read_request fd =
  let buf = Buffer.create 256 and chunk = Bytes.create 4096 in
  let rec go () =
    if Buffer.length buf > max_request then None
    else
      match Unix.read fd chunk 0 (Bytes.length chunk) with
      | 0 -> None
      | n -> (
          let piece = Bytes.sub_string chunk 0 n in
          match String.index_opt piece '\n' with
          | Some j ->
              Buffer.add_string buf (String.sub piece 0 j);
              Some (Buffer.contents buf)
          | None ->
              Buffer.add_string buf piece;
              go ())
      | exception Unix.Unix_error (Unix.EINTR, _, _) -> go ()
      | exception Unix.Unix_error _ -> None
  in
  go ()

(* one request per connection; a client that sends nothing is dropped after 10 seconds *)
let serve st fd =
  Fun.protect
    ~finally:(fun () -> try Unix.close fd with Unix.Unix_error _ -> ())
    (fun () ->
      (try
         Unix.setsockopt_float fd Unix.SO_RCVTIMEO 10.;
         Unix.setsockopt_float fd Unix.SO_SNDTIMEO 10.
       with Unix.Unix_error _ -> ());
      match read_request fd with
      | None -> ()
      | Some line -> (
          let s = Yojson.Safe.to_string (answer st line) ^ "\n" in
          try ignore (Unix.write_substring fd s 0 (String.length s)) with Unix.Unix_error _ -> ()))

let rec loop st listener wake =
  if st.owned <> None then
    match Unix.select [ listener; wake ] [] [] (-1.) with
    | exception Unix.Unix_error (Unix.EINTR, _, _) -> loop st listener wake
    | ready, _, _ ->
        if not (List.mem wake ready) then begin
          (match Unix.accept ~cloexec:true listener with fd, _ -> serve st fd | exception Unix.Unix_error _ -> ());
          loop st listener wake
        end

(* MOTH_BUDGET_MB: a whole number of MiB from 1 to 65536; unset or empty, no budget *)
let budget_from_env () =
  match Sys.getenv_opt "MOTH_BUDGET_MB" with
  | None | Some "" -> None
  | Some v -> (
      match int_of_string_opt v with
      | Some n when is_digits v && n >= 1 && n <= 65536 -> Some n
      | _ ->
          say "ignoring MOTH_BUDGET_MB=%S: it must be a whole number from 1 to 65536" v;
          None)

let run ~home ~start_helper =
  let sock = match socket_path ~home with Ok p -> p | Error m -> failwith m in
  make_home home;
  if answers sock then failwith already_running;
  let lock = match take_lock home with Some fd -> fd | None -> failwith already_running in
  (* the lock is ours, so whatever is at the socket's path is stale *)
  (try Unix.unlink sock with Unix.Unix_error _ -> ());
  let listener = Unix.socket ~cloexec:true Unix.PF_UNIX Unix.SOCK_STREAM 0 in
  let st =
    {
      home;
      sock;
      start_helper;
      budget_mb = budget_from_env ();
      conn = None;
      target = None;
      lost = false;
      gone = false;
      seq = last_seq home;
      wrote = None;
      held = None;
      owned = Some (listener, lock);
    }
  in
  (* a signal wakes the loop through a pipe, so a request being served finishes first *)
  let wake_r, wake_w = Unix.pipe ~cloexec:true () in
  Unix.set_nonblock wake_w;
  let wake _ = try ignore (Unix.write_substring wake_w "x" 0 1) with Unix.Unix_error _ -> () in
  let before = List.map (fun s -> (s, Sys.signal s (Sys.Signal_handle wake))) [ Sys.sigterm; Sys.sigint ] in
  let before = (Sys.sigpipe, Sys.signal Sys.sigpipe Sys.Signal_ignore) :: before in
  Fun.protect
    ~finally:(fun () ->
      shutdown st;
      List.iter (fun (s, b) -> Sys.set_signal s b) before;
      Unix.close wake_r;
      Unix.close wake_w;
      say "stopped")
    (fun () ->
      Unix.bind listener (Unix.ADDR_UNIX sock);
      Unix.chmod sock 0o600;
      Unix.listen listener 16;
      say "serving %s (pid %d)" sock (Unix.getpid ());
      loop st listener wake_r)

let default_start_helper ~home ~bottle =
  (* without crossover there are no bottles to name *)
  let* _app = Crossover.find_app () in
  let* () = Crossover.check_bottle bottle in
  let* helper_exe = Helper_conn.helper_exe () in
  Helper_conn.start
    ~launch:(Helper_conn.wine_launcher ~bottle ~helper_exe)
    ~label:(sprintf "bottle \"%s\"" bottle)
    ~log:(in_home home "helper.log") ()
