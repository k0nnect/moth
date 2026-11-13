(* The stand-in game (tests/target/target.cpp), launched in the test bottle with CrossOver's wine.
   It connects back to a port we listen on, sends one manifest line with its variables, then
   answers "set <name> <value>" and "free <page>" with "ok" and "get <name>" with the value, until "quit" or until
   the connection closes: losing the connection, or never making it, makes the target exit by
   itself. Its stdout and stderr (CrossOver writes there too) go to a log and are never read.
   [start] launches it; [adopt] takes one that something else launched (moth launch). *)

type var = { name : string; vtype : string; addr : int64; value : Yojson.Safe.t }

type t = {
  (* the wine launcher, which exits once the target (its own process) has; None for an adopted
     target, whose launcher is not ours *)
  wine_pid : int option;
  fd : Unix.file_descr;
  buf : Buffer.t;
  vars : var list;
  mutable closed : bool;
}

let now = Unix.gettimeofday
let env name = match Sys.getenv_opt name with Some v when v <> "" -> v | _ -> failwith (name ^ " is not set. run this through make.")

let rec restart f = try f () with Unix.Unix_error (Unix.EINTR, _, _) -> restart f

(* true once the process has exited (and is reaped) *)
let exited pid = match restart (fun () -> Unix.waitpid [ Unix.WNOHANG ] pid) with 0, _ -> false | _ -> true | exception Unix.Unix_error _ -> true

let wait_exit pid ~timeout =
  let deadline = now () +. timeout in
  let rec go () = if exited pid then true else if now () > deadline then false else (Unix.sleepf 0.1; go ()) in
  go ()

let kill pid =
  (try Unix.kill pid Sys.sigkill with Unix.Unix_error _ -> ());
  ignore (wait_exit pid ~timeout:5.)

let chunk = Bytes.create 4096

let read_line t ~timeout =
  let deadline = now () +. timeout in
  let rec go () =
    let s = Buffer.contents t.buf in
    match String.index_opt s '\n' with
    | Some i ->
        Buffer.clear t.buf;
        Buffer.add_string t.buf (String.sub s (i + 1) (String.length s - i - 1));
        Some (if i > 0 && s.[i - 1] = '\r' then String.sub s 0 (i - 1) else String.sub s 0 i)
    | None -> (
        let left = deadline -. now () in
        if left <= 0. then None
        else
          match restart (fun () -> Unix.select [ t.fd ] [] [] left) with
          | [], _, _ -> go ()
          | _ -> (
              match restart (fun () -> Unix.read t.fd chunk 0 (Bytes.length chunk)) with
              | 0 -> None
              | n ->
                  Buffer.add_subbytes t.buf chunk 0 n;
                  go ()
              | exception Unix.Unix_error _ -> None))
  in
  go ()

let send t line =
  let s = line ^ "\n" in
  let rec go off = if off < String.length s then go (off + restart (fun () -> Unix.write_substring t.fd s off (String.length s - off))) in
  try go 0 with Unix.Unix_error (e, _, _) -> failwith (Printf.sprintf "the target's connection broke (%s)" (Unix.error_message e))

let ask t line =
  if t.closed then failwith "the target has quit";
  send t line;
  match read_line t ~timeout:30. with Some reply -> reply | None -> failwith (Printf.sprintf "the target did not answer %S" line)

let var_of_json j =
  let open Yojson.Safe.Util in
  { name = to_string (member "name" j); vtype = to_string (member "type" j); addr = Int64.of_string (to_string (member "addr" j)); value = member "value" j }

(* waits for the target to connect, giving up early if wine (when it is ours) exits first *)
let rec accept_from listener ~wine_pid ~deadline =
  if Option.fold ~none:false ~some:exited wine_pid then failwith "the target exited before it connected"
  else if now () > deadline then None
  else
    match restart (fun () -> Unix.select [ listener ] [] [] 0.5) with
    | [], _, _ -> accept_from listener ~wine_pid ~deadline
    | _ -> Some (fst (restart (fun () -> Unix.accept ~cloexec:true listener)))

(* the first launch in a bottle can take half a minute, and a large filler adds its own time *)
let connect_timeout = 180.

(* a port on the loopback to listen on, given to [f]; closed once [f] is done *)
let listening f =
  let listener = Unix.socket ~cloexec:true Unix.PF_INET Unix.SOCK_STREAM 0 in
  Fun.protect
    ~finally:(fun () -> Unix.close listener)
    (fun () ->
      Unix.bind listener (Unix.ADDR_INET (Unix.inet_addr_loopback, 0));
      Unix.listen listener 1;
      let port = match Unix.getsockname listener with Unix.ADDR_INET (_, p) -> p | _ -> assert false in
      f listener port)

(* the target started with [port] as its first argument: its connection and its manifest. On
   failure wine, when it is ours, is killed. *)
let connected listener ~wine_pid =
  let kill_wine () = Option.iter kill wine_pid in
  match accept_from listener ~wine_pid ~deadline:(now () +. connect_timeout) with
  | exception e ->
      kill_wine ();
      raise e
  | None ->
      kill_wine ();
      failwith (Printf.sprintf "the target did not connect within %.0fs" connect_timeout)
  | Some fd -> (
      let t = { wine_pid; fd; buf = Buffer.create 4096; vars = []; closed = false } in
      let fail m =
        Unix.close fd;
        kill_wine ();
        failwith m
      in
      match read_line t ~timeout:60. with
      | None -> fail "the target sent no manifest"
      | Some line -> (
          match List.map var_of_json (Yojson.Safe.Util.(to_list (member "vars" (Yojson.Safe.from_string line)))) with
          | vars -> { t with vars }
          | exception e -> fail ("the target's manifest is not readable: " ^ Printexc.to_string e)))

(* Launches [exe] with the given filler sizes in MiB (0: none) and waits for its manifest. It
   needs WINE and MOTH_TEST_BOTTLE. [log] gets wine's output. *)
let start ?(filler_small_mb = 0) ?(filler_tiny_mb = 0) ?(filler_half_mb = 0) ?(log = "/dev/null") ~exe ~filler_mb () =
  if not (Sys.file_exists exe) then failwith (exe ^ " not found. run make helper.");
  listening (fun listener port ->
      let filler flag mb = if mb > 0 then [ flag; string_of_int mb ] else [] in
      let argv =
        Array.of_list
          ([ env "WINE"; "--bottle"; env "MOTH_TEST_BOTTLE"; "--workdir"; Filename.dirname exe; exe; string_of_int port ]
          @ filler "--filler-mb" filler_mb @ filler "--filler-small-mb" filler_small_mb @ filler "--filler-tiny-mb" filler_tiny_mb
          @ filler "--filler-half-mb" filler_half_mb)
      in
      let null = Unix.openfile "/dev/null" [ Unix.O_RDONLY; Unix.O_CLOEXEC ] 0 in
      let out = Unix.openfile log [ Unix.O_WRONLY; Unix.O_CREAT; Unix.O_APPEND; Unix.O_CLOEXEC ] 0o644 in
      let wine_pid = Fun.protect ~finally:(fun () -> Unix.close null; Unix.close out) (fun () -> Unix.create_process argv.(0) argv null out out) in
      connected listener ~wine_pid:(Some wine_pid))

(* A target that something else starts, such as moth launch: listens first, hands [launch] the
   port to give the target as its first argument, then waits for the target to connect and for
   its manifest, as [start] does. Its process is not ours: [stop] and [quit] wait for it to close
   its end of the connection, which it does as it exits. *)
let adopt launch =
  listening (fun listener port ->
      launch port;
      connected listener ~wine_pid:None)

let vars t = t.vars
let var t name = match List.find_opt (fun v -> v.name = name) t.vars with Some v -> v | None -> failwith ("the target has no variable " ^ name)

let set t name value =
  match ask t (Printf.sprintf "set %s %s" name value) with "ok" -> () | r -> failwith (Printf.sprintf "the target refused set %s %s: %s" name value r)

let get t name = ask t ("get " ^ name)

(* decommits one page of the target's small filler (its large one when it has no small one) *)
let free t page =
  match ask t (Printf.sprintf "free %d" page) with "ok" -> () | r -> failwith (Printf.sprintf "the target refused free %d: %s" page r)

(* true once the target has closed its end of the connection, within [timeout] *)
let hung_up t ~timeout =
  let deadline = now () +. timeout in
  let rec go () =
    let left = deadline -. now () in
    left > 0.
    &&
    match restart (fun () -> Unix.select [ t.fd ] [] [] left) with
    | [], _, _ -> go ()
    | _ -> ( match restart (fun () -> Unix.read t.fd chunk 0 (Bytes.length chunk)) with 0 -> true | _ -> go () | exception Unix.Unix_error _ -> true)
  in
  go ()

(* Lets the target go: closing the connection makes it exit. Waits for wine to end, and kills
   the launcher when it does not. An adopted target is waited for until it closes its end (30
   seconds at most). Safe to call more than once. *)
let stop t =
  if not t.closed then begin
    t.closed <- true;
    match t.wine_pid with
    | Some pid ->
        (try Unix.close t.fd with Unix.Unix_error _ -> ());
        if not (wait_exit pid ~timeout:30.) then kill pid
    | None ->
        (try Unix.shutdown t.fd Unix.SHUTDOWN_SEND with Unix.Unix_error _ -> ());
        ignore (hung_up t ~timeout:30.);
        (try Unix.close t.fd with Unix.Unix_error _ -> ())
  end

(* tells the target to quit and waits until its process has ended *)
let quit t =
  if not t.closed then begin
    (try send t "quit" with Failure _ -> ());
    stop t
  end
