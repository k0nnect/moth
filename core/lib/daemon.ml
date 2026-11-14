type start_helper = bottle:string -> (Helper_api.helper, string) result
type launch = bottle:string -> exe:string -> args:string list -> (unit, string) result

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

(* a connection that sent watch. The feed is written to it without waiting; [backlog] is watch's
   reply and the stored events it asked for, sent up to [sent] so far *)
type watcher = { wfd : Unix.file_descr; backlog : string; mutable sent : int }

type state = {
  home : string;
  sock : string;
  start_helper : start_helper;
  launch : launch;
  budget_mb : int option; (* from MOTH_BUDGET_MB, for every session *)
  snapshot_mb : int option; (* from MOTH_SNAPSHOT_MB, for every session *)
  mutable conn : (string * Helper_api.helper) option; (* the helper and the bottle it runs in *)
  mutable target : target option; (* always in the helper's bottle *)
  mutable lost : bool; (* the helper said helper_lost during this request *)
  mutable gone : bool; (* the helper said target_gone during this request *)
  mutable seq : int; (* the newest event's number *)
  mutable wrote : Yojson.Safe.t option; (* the write this request made, for its event *)
  mutable held : Yojson.Safe.t option; (* whether this request's set held, for its event *)
  mutable closed : bool; (* this request's status found the game closed *)
  mutable quiet : bool; (* the agent is not told what the person does; kept in [home]/quiet *)
  mutable owned : (Unix.file_descr * Unix.file_descr) option; (* listener and lock, while serving *)
  mutable watchers : watcher list; (* oldest first *)
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
      let t = { proc; bottle; session = Session.create ?budget_mb:st.budget_mb ?snapshot_mb:st.snapshot_mb h } in
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
  ( `List (List.map (fun (k : Session.kept) ->
         `Assoc
           [
             ("name", `String k.name);
             ("addr", hex k.kaddr);
             ("tag", `String k.ktag);
             ("shown", `String k.kshown);
             ("set_to", match k.set_to with Some v -> `String v | None -> `Null);
             ("put_back", `Bool k.put_back);
             ("note", `String (Render.kept_note k));
           ])
         ks),
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

(* the client protocol number, in [status --json] as proto *)
let protocol = 2

(* Asks the helper whether the game is still there. When it has exited, the target goes, and its
   hunt, kept values and writes with it; the helper stays, and lets go of the game's candidates
   now rather than at the next attach (if it can't, the answer is the same). *)
let status st =
  let with_proto = function Ok (`Assoc f, t) -> Ok (`Assoc (f @ [ ("proto", `Int protocol); ("quiet", `Bool st.quiet) ]), t) | r -> r in
  with_proto @@
  match (st.target, st.conn) with
  | Some t, Some (_, h) -> (
      match h.status () with
      | Ok None ->
          st.target <- None;
          st.closed <- true;
          ignore (h.detach ());
          Ok (`Assoc [ ("attached", `Null); ("hunt", `Null); ("closed", `String t.proc.name) ], sprintf "not attached. %s closed." t.proc.name)
      | Error { code = "helper_lost"; _ } -> Error "lost contact with moth's helper. attach again."
      | Error e -> Error (message e)
      | Ok (Some _) -> (
          let attached = `Assoc [ ("pid", `Int t.proc.pid); ("name", `String t.proc.name); ("bits", `Int t.proc.bits); ("bottle", `String t.bottle) ] in
          match Session.current t.session with
          | None -> Ok (`Assoc [ ("attached", attached); ("hunt", `Null) ], attached_line t)
          | Some o ->
              (* a snapshot no step has narrowed: every aligned place counts, and nothing lists yet *)
              let blind = Session.blind t.session in
              let hunt =
                `Assoc
                  [
                    ("main", `Int o.main);
                    ("wide", `Int o.wide);
                    ("by_tag", `List (List.map (fun (tag, c) -> `List [ `String tag; `Int c ]) o.by_tag));
                    ("can_compare", `Bool (Session.can_compare o));
                    ("groups", `List (List.map group_json o.groups));
                    ("blind", `Bool blind);
                  ]
              in
              Ok (`Assoc [ ("attached", attached); ("hunt", hunt) ], attached_line t ^ "\n" ^ if blind then Render.no_step_yet else Render.counts o)))
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

(* ---- launch and games: [home]/games.json, newest first ---- *)

type game = { name : string; bottle : string; path : string; last : float }

let max_games = 20
let games_file st = in_home st.home "games.json"

(* an unreadable file counts as empty *)
let read_games st =
  let open Yojson.Safe.Util in
  try
    Yojson.Safe.from_file (games_file st)
    |> to_list
    |> List.filter_map (fun g ->
           try Some { name = to_string (member "name" g); bottle = to_string (member "bottle" g); path = to_string (member "path" g); last = to_float (member "last" g) }
           with Type_error _ -> None)
  with Sys_error _ | Yojson.Json_error _ | Type_error _ -> []

let write_games st games =
  let json = `List (List.map (fun g -> `Assoc [ ("name", `String g.name); ("bottle", `String g.bottle); ("path", `String g.path); ("last", `Float g.last) ]) games) in
  let tmp = games_file st ^ ".tmp" in
  try
    Yojson.Safe.to_file tmp json;
    Unix.rename tmp (games_file st)
  with Sys_error _ | Unix.Unix_error _ -> say "couldn't write %s" (games_file st)

let remember st ~bottle ~path =
  let g = { name = Filename.basename path; bottle; path; last = Unix.time () } in
  let others = List.filter (fun o -> not (o.bottle = bottle && o.path = path)) (read_games st) in
  write_games st (List.filteri (fun i _ -> i < max_games) (g :: others))

let launch_usage = "launch needs a bottle and a program: moth launch --bottle <bottle> <full path to game.exe>"

let launch_game st ~bottle ~path ~args =
  let* () =
    if Filename.is_relative path || not (String.lowercase_ascii (Filename.extension path) = ".exe") then
      Error (sprintf "launch needs the full path to a windows program (.exe): %s" path)
    else if not (Sys.file_exists path) then Error (sprintf "no such file: %s" path)
    else Ok ()
  in
  let* () = st.launch ~bottle ~exe:path ~args in
  remember st ~bottle ~path;
  let name = Filename.basename path in
  let m = sprintf "started %s in %s. when it is up: moth attach --bottle \"%s\" %s" name bottle bottle name in
  Ok (`Assoc [ ("message", `String m); ("name", `String name); ("bottle", `String bottle); ("path", `String path) ], m)

let games st =
  match read_games st with
  | [] -> (`List [], "nothing launched from moth yet. start a game with: moth launch --bottle <bottle> <full path to game.exe>")
  | gs ->
      let w f = List.fold_left (fun w g -> max w (String.length (f g))) 0 gs in
      let wn = w (fun g -> g.name) and wb = w (fun g -> g.bottle) in
      ( `List (List.map (fun g -> `Assoc [ ("name", `String g.name); ("bottle", `String g.bottle); ("path", `String g.path) ]) gs),
        String.concat "\n" (List.map (fun g -> sprintf "%-*s  %-*s  %s" wn g.name wb g.bottle g.path) gs) )

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

let seq_of ev = Yojson.Safe.Util.(to_int (member "seq" ev))
let last_seq home = match read_events home with ev :: _ -> seq_of ev | [] -> 0
let first_line s = match String.index_opt s '\n' with Some i -> String.sub s 0 i | None -> s

(* ---- the feed ---- *)

let close_quietly fd = try Unix.close fd with Unix.Unix_error _ -> ()

let let_go st w =
  st.watchers <- List.filter (fun x -> x != w) st.watchers;
  close_quietly w.wfd

let owed w = w.sent < String.length w.backlog

(* as much of the backlog as the socket takes now *)
let send_backlog st w =
  match Unix.write_substring w.wfd w.backlog w.sent (String.length w.backlog - w.sent) with
  | n -> w.sent <- w.sent + n
  | exception Unix.Unix_error ((Unix.EAGAIN | Unix.EWOULDBLOCK | Unix.EINTR), _, _) -> ()
  | exception Unix.Unix_error _ -> let_go st w

(* a watcher has nothing more to say after watch: what it sends is dropped, and its end (or a
   half-close) lets it go *)
let hear st w =
  let buf = Bytes.create 512 in
  match Unix.read w.wfd buf 0 (Bytes.length buf) with
  | 0 -> let_go st w
  | _ -> ()
  | exception Unix.Unix_error ((Unix.EAGAIN | Unix.EWOULDBLOCK | Unix.EINTR), _, _) -> ()
  | exception Unix.Unix_error _ -> let_go st w

(* A line to every watcher, whole and at once. A watcher whose socket can't take all of it now, or
   that is still owed part of its backlog, is let go (it watches again from the newest event it
   has), so a request never waits on a watcher. *)
let feed st j =
  let line = Yojson.Safe.to_string j ^ "\n" in
  let n = String.length line in
  List.iter
    (fun w ->
      let whole = (not (owed w)) && match Unix.write_substring w.wfd line 0 n with k -> k = n | exception Unix.Unix_error _ -> false in
      if not whole then let_go st w)
    st.watchers

(* one line of events.jsonl: the command, its arguments and its options (when any were given),
   how it went, and for a write, whether it held and what it wrote. Watchers are sent it after. *)
let record st ~actor ~act ~cmd ~args ~opts result =
  st.seq <- st.seq + 1;
  let ok, summary = match result with Ok (_, text) -> (true, first_line text) | Error e -> (false, first_line e) in
  let ev =
    `Assoc
      ([
         ("seq", `Int st.seq);
         ("ts", `Float (Float.round (Unix.gettimeofday () *. 1000.) /. 1000.));
         ("actor", `String actor);
         ("act", `Bool act);
         ("cmd", `String cmd);
         ("args", `List (List.map (fun a -> `String a) args));
       ]
      @ (if opts = [] then [] else [ ("opts", `Assoc (List.map (fun (k, v) -> (k, `String v)) opts)) ])
      @ [ ("ok", `Bool ok); ("summary", `String summary) ]
      @ (match st.held with Some h -> [ ("held", h) ] | None -> [])
      @ match st.wrote with Some w -> [ ("write", w) ] | None -> [])
  in
  (try
     let oc = open_out_gen [ Open_wronly; Open_creat; Open_append; Open_binary ] 0o600 (in_home st.home "events.jsonl") in
     Fun.protect ~finally:(fun () -> close_out_noerr oc) (fun () -> output_string oc (Yojson.Safe.to_string ev ^ "\n"); close_out oc)
   with Sys_error m -> say "couldn't write the event log: %s" m);
  feed st (`Assoc [ ("event", ev) ])

let log st opts =
  let n = match List.assoc_opt "n" opts with Some s -> int_of_string s | None -> 20 in
  let evs = List.filteri (fun i _ -> i < n) (read_events st.home) in
  (`List evs, if evs = [] then "nothing logged yet" else String.concat "\n" (List.filter_map event_line evs))

(* ---- commands ---- *)

(* quiet on|off, kept in [home]/quiet (present means on) so a restart keeps it; no argument says
   which it is *)
let quiet st args =
  let file = in_home st.home "quiet" in
  let set on =
    try
      if on then close_out (open_out_gen [ Open_wronly; Open_creat; Open_trunc ] 0o600 file) else if Sys.file_exists file then Sys.remove file;
      st.quiet <- on;
      Ok ()
    with Sys_error m -> Error (sprintf "couldn't keep quiet in %s: %s" file m)
  in
  match args with
  | [ "on" ] ->
      let* () = set true in
      Ok (said "quiet: the agent is not told what you do. it can still look: moth log")
  | [ "off" ] ->
      let* () = set false in
      Ok (said "the agent is told what you do again")
  | _ -> Ok (said (if st.quiet then "quiet is on" else "quiet is off"))

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
  (* --at pins a row to the address it showed; it is checked here, before anything runs *)
  let at_ok =
    match List.assoc_opt "at" opts with
    | None -> Ok ()
    | Some a ->
        let row_form = match (cmd, args) with ("keep", ([ r ] | [ r; _ ])) | ("set", [ r; _ ]) -> row_number r <> None | _ -> false in
        if cmd <> "keep" && cmd <> "set" then Error "--at goes with keep and set"
        else if not row_form then Error "--at goes with a row number"
        else need (address a <> None) "--at needs an address like 0x7ffe1234"
  in
  let* () = at_ok in
  (* --kind, and --all on a row write, say how to find the row --at names *)
  let pinned = List.mem_assoc "at" opts in
  let* () = need (pinned || not (List.mem_assoc "kind" opts)) "--kind goes with --at" in
  let* () = need (pinned || not ((cmd = "set" || cmd = "keep") && List.mem_assoc "all" opts)) "--all goes with --at" in
  match cmd with
  | "bottles" | "status" | "kept" | "undo" | "detach" | "stop" -> none
  | "ps" ->
      let* () = need (List.mem_assoc "bottle" opts) "ps needs a bottle: moth ps --bottle <bottle>" in
      none
  | "attach" -> need (List.length args = 1 && List.mem_assoc "bottle" opts) "attach needs a bottle and a process: moth attach --bottle <bottle> <game.exe>"
  | "launch" -> need (args <> [] && List.mem_assoc "bottle" opts) launch_usage
  | "games" -> none
  | "find" -> need (List.length args = 1) "find needs a number"
  | "snapshot" -> none
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
  | "watch" ->
      let* () = none in
      need (whole "after" (fun _ -> true)) "watch --after needs an event number"
  | "quiet" -> need (match args with [] | [ ("on" | "off") ] -> true | _ -> false) "quiet takes on or off"
  | _ -> Error (sprintf "unknown command: %s. run: moth help" cmd)

let relations = [ ("down", Session.Down); ("up", Session.Up); ("same", Session.Same); ("changed", Session.Changed) ]

(* [record]: false when the request asked to leave no trace; such a results listing is not the
   one row numbers are taken from *)
let handle st ~record ~cmd ~args ~opts =
  let* () = check_usage ~cmd ~args ~opts in
  let bottle () = List.assoc "bottle" opts in
  let all = List.assoc_opt "all" opts = Some "true" in
  let at = Option.bind (List.assoc_opt "at" opts) address and kind = List.assoc_opt "kind" opts in
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
  | "snapshot", _ ->
      let* s = session () in
      Result.map outcome (Session.snapshot s)
  | "next", [ a ] ->
      let* s = session () in
      Result.map outcome
        (match List.assoc_opt (String.lowercase_ascii a) relations with Some r -> Session.next_relation s r | None -> Session.next_number s a)
  | "results", _ ->
      let* s = session () in
      let near = List.assoc_opt "near" opts in
      let given k default = Option.fold ~none:default ~some:int_of_string (List.assoc_opt k opts) in
      Result.map (listing ?near ~all) (Session.results ~remember:record s ~all ~from:(given "from" 1) ~limit:(given "n" 20) ~near)
  | "keep", first :: rest ->
      let* s = session () in
      let name = function [ n ] -> Some n | _ -> None in
      Result.map said
        (match (address first, rest) with
        | Some addr, number :: more -> Session.keep_at s ~addr number ~name:(name more)
        | _ -> Session.keep_row ?at ?kind ~all s ~row:(Option.get (row_number first)) ~name:(name rest))
  | "kept", _ ->
      let* s = session () in
      Result.map kept (Session.kept s)
  | "forget", [ name ] ->
      let* s = session () in
      Result.map said (Session.forget s name)
  | "set", [ target; v ] ->
      let* s = session () in
      let* d = match row_number target with Some row -> Session.set ?at ?kind ~all s ~row v | None -> Session.set_kept s ~name:target v in
      Ok (set_done st d)
  | "undo", _ ->
      let* s = session () in
      let* text, w = Session.undo s in
      st.wrote <- Some (written_json w);
      Ok (said text)
  | "launch", path :: args -> launch_game st ~bottle:(bottle ()) ~path ~args
  | "games", _ -> Ok (games st)
  | "detach", _ -> Ok (said (detach st))
  | "log", _ -> Ok (log st opts)
  | "quiet", _ -> quiet st args
  | "stop", _ ->
      shutdown st;
      Ok (said "stopped")
  | _ -> Error (sprintf "unknown command: %s. run: moth help" cmd)

(* ---- the protocol ---- *)

(* [record]: false when the request asked to leave no trace; [proto]: the client's protocol number,
   when it sent one *)
type request = { rid : Yojson.Safe.t; actor : string; cmd : string; args : string list; opts : (string * string) list; record : bool; proto : int option }

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
          let record = List.assoc_opt "record" f <> Some (`Bool false) in
          let proto = match List.assoc_opt "proto" f with Some (`Int n) -> Some n | _ -> None in
          Ok { rid; actor; cmd; args; opts; record; proto }
      | _ -> Error (rid, "bad request: no cmd"))
  | _ -> Error (`Null, "bad request: not an object")

let reply rid = function
  | Ok (result, text) -> `Assoc [ ("id", rid); ("ok", `Bool true); ("result", result); ("text", `String text) ]
  | Error e -> `Assoc [ ("id", rid); ("ok", `Bool false); ("error", `String e) ]

let send fd j =
  let s = Yojson.Safe.to_string j ^ "\n" in
  try ignore (Unix.write_substring fd s 0 (String.length s)) with Unix.Unix_error _ -> ()

let internal_error st cmd e =
  let e = Printexc.to_string e in
  say "internal error in %s: %s" cmd e;
  sprintf "moth hit an internal error: %s. see %s" e (in_home st.home "core.log")

(* the commands that change nothing, so may be asked to leave no trace *)
let reads = [ "bottles"; "ps"; "status"; "results"; "kept"; "log"; "games" ]

let other_build = "this moth is from another build than its background service. stop the service and it starts again by itself: moth stop"

(* A client must send this build's protocol number, or it is not served: an older moth would not
   say who is speaking. stop and status are served whatever it sends, so any moth can see and stop
   any service, and the window can tell that a service is from another build. *)
let same_build r = r.proto = Some protocol || List.mem r.cmd [ "stop"; "status" ]

(* A recorded request is told to watchers as it starts and again, as its event, once it ends. An
   unrecorded one takes no seq and tells nobody, unless it made the daemon let go of the game or
   the helper (the game closed, target_gone, helper_lost): that one is recorded all the same, so
   watchers learn why the game went, and its event comes with no busy line before it. *)
let answer st = function
  | Error (rid, msg) -> reply rid (Error msg)
  | Ok r when not (same_build r) -> reply r.rid (Error other_build)
  | Ok r when not (r.record || List.mem r.cmd reads) -> reply r.rid (Error "only commands that change nothing can go unrecorded")
  | Ok r ->
      if r.record then feed st (`Assoc [ ("busy", `Assoc [ ("actor", `String r.actor); ("cmd", `String r.cmd); ("args", `List (List.map (fun a -> `String a) r.args)) ]) ]);
      let result = try handle st ~record:r.record ~cmd:r.cmd ~args:r.args ~opts:r.opts with e -> Error (internal_error st r.cmd e) in
      let lost_the_game = st.closed || st.lost || st.gone in
      settle st;
      if r.record || lost_the_game then record st ~actor:r.actor ~act:(lost_the_game || not (List.mem r.cmd reads)) ~cmd:r.cmd ~args:r.args ~opts:r.opts result;
      st.wrote <- None;
      st.held <- None;
      st.closed <- false;
      reply r.rid result

let max_watchers = 16
let max_backlog = 1000

(* [watch]: its reply, then the stored events after [after] (the newest 1,000 at most, oldest
   first) as event lines, then the feed. Never recorded. True when [fd] became a watcher; when
   refused it has its error and is the caller's to close. *)
let add_watcher st fd r =
  let refuse e =
    send fd (reply r.rid (Error e));
    false
  in
  match check_usage ~cmd:r.cmd ~args:r.args ~opts:r.opts with
  | _ when not (same_build r) -> refuse other_build
  | Error e -> refuse e
  | Ok () when List.length st.watchers >= max_watchers -> refuse "too many watchers"
  | Ok () ->
      let stored =
        match List.assoc_opt "after" r.opts with
        | None -> []
        | Some k ->
            let k = int_of_string k in
            List.rev (List.filteri (fun i _ -> i < max_backlog) (List.filter (fun ev -> seq_of ev > k) (read_events st.home)))
      in
      let line j = Yojson.Safe.to_string j ^ "\n" in
      let backlog = line (reply r.rid (Ok (`Assoc [ ("seq", `Int st.seq) ], ""))) :: List.map (fun ev -> line (`Assoc [ ("event", ev) ])) stored in
      Unix.set_nonblock fd;
      let w = { wfd = fd; backlog = String.concat "" backlog; sent = 0 } in
      st.watchers <- st.watchers @ [ w ];
      send_backlog st w;
      true

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

(* one request per connection, except that a watcher's stays open for the feed; a client that
   sends nothing is dropped after 10 seconds *)
let serve st fd =
  let watching = ref false in
  Fun.protect
    ~finally:(fun () -> if not !watching then close_quietly fd)
    (fun () ->
      (try
         Unix.setsockopt_float fd Unix.SO_RCVTIMEO 10.;
         Unix.setsockopt_float fd Unix.SO_SNDTIMEO 10.
       with Unix.Unix_error _ -> ());
      match read_request fd with
      | None -> ()
      | Some line -> (
          match parse_request line with
          | Ok ({ cmd = "watch"; _ } as r) -> ( try watching := add_watcher st fd r with e -> send fd (reply r.rid (Error (internal_error st r.cmd e))))
          | parsed -> send fd (answer st parsed)))

(* Requests are served one at a time. Watchers wait in the same select: one that has gone is let
   go before the next connection is taken (so its place is free), and one still owed its backlog
   is sent more as its socket takes it. *)
let rec loop st listener wake =
  if st.owned <> None then
    let watcher_fds = List.map (fun w -> w.wfd) st.watchers in
    let owing = List.filter_map (fun w -> if owed w then Some w.wfd else None) st.watchers in
    match Unix.select (listener :: wake :: watcher_fds) owing [] (-1.) with
    | exception Unix.Unix_error (Unix.EINTR, _, _) -> loop st listener wake
    | ready, writable, _ ->
        if not (List.mem wake ready) then begin
          List.iter (fun w -> if List.mem w.wfd ready then hear st w) st.watchers;
          List.iter (fun w -> if List.mem w.wfd writable then send_backlog st w) st.watchers;
          if List.mem listener ready then (match Unix.accept ~cloexec:true listener with fd, _ -> serve st fd | exception Unix.Unix_error _ -> ());
          loop st listener wake
        end

(* an env var holding a whole number of MiB from 1 to [max]; unset or empty, none; anything
   else is ignored, with a line in the log *)
let mb_from_env name max =
  match Sys.getenv_opt name with
  | None | Some "" -> None
  | Some v -> (
      match int_of_string_opt v with
      | Some n when is_digits v && n >= 1 && n <= max -> Some n
      | _ ->
          say "ignoring %s=%S: it must be a whole number from 1 to %d" name v max;
          None)

let run ~home ~start_helper ~launch =
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
      launch;
      budget_mb = mb_from_env "MOTH_BUDGET_MB" 65536;
      snapshot_mb = mb_from_env "MOTH_SNAPSHOT_MB" 1048576;
      conn = None;
      target = None;
      lost = false;
      gone = false;
      seq = last_seq home;
      wrote = None;
      held = None;
      closed = false;
      quiet = Sys.file_exists (in_home home "quiet");
      owned = Some (listener, lock);
      watchers = [];
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
      (* only now, so a stop's event reaches them; the socket is gone, so none can watch again *)
      List.iter (fun w -> close_quietly w.wfd) st.watchers;
      st.watchers <- [];
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

(* the program under CrossOver's wine, in its own session with stdin from /dev/null and its output
   appended to [home]/launch.log; not waited for *)
let default_launch ~home ~bottle ~exe ~args =
  let* app = Crossover.find_app () in
  let* () = Crossover.check_bottle bottle in
  let argv = Crossover.wine_argv ~app ~bottle ~exe args in
  let log = in_home home "launch.log" in
  (* a new session, so the game outlives the daemon and holds no terminal; a grandchild, so it is
     nobody's zombie when it ends *)
  match Unix.fork () with
  | exception Unix.Unix_error (e, _, _) -> Error (sprintf "could not start wine (%s). see %s" (Unix.error_message e) log)
  | 0 ->
      (try
         ignore (Unix.setsid ());
         if Unix.fork () = 0 then begin
           let null = Unix.openfile "/dev/null" [ Unix.O_RDONLY ] 0 in
           let out = Unix.openfile log [ Unix.O_WRONLY; Unix.O_CREAT; Unix.O_APPEND ] 0o644 in
           Unix.dup2 null Unix.stdin;
           Unix.dup2 out Unix.stdout;
           Unix.dup2 out Unix.stderr;
           Unix.close null;
           Unix.close out;
           Unix.execv argv.(0) argv
         end
       with _ -> ());
      Unix._exit 0
  | pid ->
      let rec wait () = match Unix.waitpid [] pid with _ -> () | exception Unix.Unix_error (Unix.EINTR, _, _) -> wait () in
      wait ();
      Ok ()
