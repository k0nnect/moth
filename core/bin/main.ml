open Moth_core

let usage =
  {|usage: moth <command> [--json]

  bottles                              list crossover bottles
  ps --bottle <bottle>                 list the windows programs running in a bottle
  attach --bottle <bottle> <game.exe>  attach to a running game, by name or pid
  launch --bottle <bottle> <game.exe path> [args]  start a game in a bottle
  games                                games started from moth before
  status                               what moth is attached to, and the hunt so far
  find <number>                        start a hunt for a number the game shows
  snapshot                             start a hunt with no number: remember everything, then narrow by how the value moves
  next <number>                        narrow the hunt to the number it shows now
  next down|up|same|changed            narrow the hunt by how the number moved
  results [--all] [-n <rows>] [--from <row>] [--near <name|0xaddr>]  list the matches left (-n from 1 to 200, default 20)
  keep <#n> [name] [--at <0xaddr>]     keep a result
  keep <0xaddr> <number> [name]        keep an address that shows a number
  kept                                 the kept values, read now
  forget <name>                        drop a kept value
  set <#n|name> <number> [--at <0xaddr>]  write a number into result #n, or into a kept value
  undo                                 put back what the last set replaced
  detach                               let go of the game
  log [-n <count>]                     recent commands, newest first
  watch [--after <seq>]                watch commands as they happen, from anyone
  quiet [on|off]                       stop or resume telling the agent what you do
  agent                                start claude code on this moth session
  stop                                 stop moth's background service
  daemon                               run the background service in the foreground

  a result number can be written 3 or #3. in a script, write 3 or quote '#3'.
  set and keep take #n from the last results you listed. with --at <0xaddr>, #n is
  looked up again as results lists it now (not --near), and must still be at that
  address; --kind <interp_id> (from results --json) pins its kind too, and --all
  counts the rarer forms, as results --all does. --kind and --all go with --at.
  --json prints the result as json.
|}

let fail code m =
  prerr_endline m;
  exit code

(* a feed line as text: an event as moth log prints it, a start as working: <cmd> <args> (<actor>) *)
let feed_text line =
  let open Yojson.Safe.Util in
  try
    match member "busy" line with
    | `Assoc _ as b ->
        let words = to_string (member "cmd" b) :: List.map to_string (to_list (member "args" b)) in
        Some (Printf.sprintf "working: %s (%s)" (String.concat " " words) (to_string (member "actor" b)))
    | _ -> Daemon.event_line (member "event" line)
  with Type_error _ -> None

(* [moth watch]: the feed until the daemon goes. If the daemon lets go of it while still running
   (it fell behind), it watches again from the newest event it printed. Until it has printed one that
   is --after when one was given, else the seq the first reply gave. *)
let watch ~json ~after =
  let home = Client.home () in
  let newest = ref after in
  let show line =
    (match Yojson.Safe.Util.(member "event" line |> member "seq") with
    | `Int n -> newest := Some n
    | _ | (exception Yojson.Safe.Util.Type_error _) -> ());
    (if json then print_endline (Yojson.Safe.to_string line) else Option.iter print_endline (feed_text line));
    true
  in
  let rec follow ~first =
    match Client.watch ~home ?after:!newest ~started:(fun seq -> if !newest = None then newest := Some seq) show with
    | Ok () -> follow ~first:false
    | Error m when m = Client.not_running && not first ->
        print_endline (if json then Yojson.Safe.to_string (`Assoc [ ("message", `String "moth stopped") ]) else "moth stopped")
    | Error m -> fail 1 m
  in
  (* stdout closed (moth watch | head) ends it quietly *)
  try follow ~first:true with Sys_error _ -> ()

let read_file path =
  match open_in_bin path with
  | exception Sys_error _ -> None
  | ic -> Fun.protect ~finally:(fun () -> close_in_noerr ic) (fun () -> try Some (String.trim (really_input_string ic (in_channel_length ic))) with Sys_error _ | End_of_file -> None)

(* through a temporary name of the writer's own, so a reader never sees half a text and two
   writers never share a file; readable by the owner alone, as the session's other files are *)
let write_file path text =
  let tmp = Printf.sprintf "%s.%d.tmp" path (Unix.getpid ()) in
  try
    let oc = open_out_gen [ Open_wronly; Open_creat; Open_trunc; Open_binary ] 0o600 tmp in
    Fun.protect ~finally:(fun () -> close_out_noerr oc) (fun () -> output_string oc text);
    Sys.rename tmp path
  with Sys_error _ -> ( try Sys.remove tmp with Sys_error _ -> ())

let process_alive pid = match Unix.kill pid 0 with () -> true | exception Unix.Unix_error (Unix.EPERM, _, _) -> true | exception Unix.Unix_error _ -> false

(* a session folder's name: a process id, decimal digits and above 0 *)
let session_pid name =
  match int_of_string_opt name with Some p when p > 0 && String.for_all (function '0' .. '9' -> true | _ -> false) name -> Some p | _ -> None

(* how long [moth wait] gives a service to answer; MOTH_WAIT_TIMEOUT (seconds) is for tests *)
let answer_limit () = match Option.bind (Sys.getenv_opt "MOTH_WAIT_TIMEOUT") float_of_string_opt with Some t when t > 0. -> t | _ -> 30.

(* [moth wait --session <dir>], run by the agent's hooks as each turn starts (UserPromptSubmit)
   and ends (Stop), the newest taking over from the one before. It follows the feed, and when the
   person has done something (see Wait) prints the note to stderr and exits 2, which wakes the
   agent: 2 means that and nothing else. Every other end is exit 0 with nothing printed: the
   session is gone (its pid, the folder's name, is not alive, or the folder is), a newer waiter
   took over [dir]/waiter, or the service is from another build (its status gives another proto,
   or it refuses the watch with Daemon.other_build). It never starts a service: with none
   answering (or none answering in time) it asks again every second, and goes on telling what it
   holds meanwhile. *)
let wait ~session =
  let home = Client.home () in
  let limit = answer_limit () in
  let dir =
    let rec strip d = if String.length d > 1 && d.[String.length d - 1] = '/' then strip (String.sub d 0 (String.length d - 1)) else d in
    strip session
  in
  let in_dir f = Filename.concat dir f in
  let session_alive () =
    (try Sys.file_exists dir && Sys.is_directory dir with Sys_error _ -> false)
    && match session_pid (Filename.basename dir) with Some p -> process_alive p | None -> false
  in
  let me = string_of_int (Unix.getpid ()) in
  if not (session_alive ()) then exit 0;
  write_file (in_dir "waiter") me;
  let mine () = read_file (in_dir "waiter") = Some me in
  let cursor_file () = Option.bind (read_file (in_dir "cursor")) Wait.parse_cursor in
  let written = ref (cursor_file ()) in
  let keep_cursor t =
    let now = (Wait.cursor t, Wait.quiet t) in
    if !written <> Some now then begin
      write_file (in_dir "cursor") (Wait.cursor_text (fst now) (snd now));
      written := Some now
    end
  in
  let w = ref None in
  (* the first look at the service failed: end this watch and try again *)
  let failed = ref false in
  (* the session, the waiter and what is held: leaves for good when the session is gone or
     another waiter has taken over, tells (and exits 2) when there is something to tell, and
     says whether the watch is to go on *)
  let look () =
    if not (session_alive () && mine ()) then exit 0;
    (match !w with
    | None -> ()
    | Some t -> (
        match Wait.check t ~now:(Unix.gettimeofday ()) with
        | Wait.Keep_waiting -> if Wait.quiet t then keep_cursor t
        | Wait.Tell { notes; _ } ->
            if not (mine ()) then exit 0;
            keep_cursor t;
            prerr_endline notes;
            exit 2));
    not !failed
  in
  let feed line =
    Option.iter (fun t -> Wait.line t ~now:(Unix.gettimeofday ()) line) !w;
    look ()
  in
  (* a second, looking every quarter of it *)
  let nap () =
    for _ = 1 to 4 do
      Unix.sleepf 0.25;
      ignore (look ())
    done
  in
  (* the unrecorded status read, once the watch is up: [proto] and [quiet], or None *)
  let first_look () =
    match Client.call ~autostart:false ~record:false ~timeout:limit ~home ~actor:"user" ~cmd:"status" ~args:[] ~opts:[] () with
    | Error _ -> None
    | Ok (status, _) ->
        let field k = match status with `Assoc f -> List.assoc_opt k f | _ -> None in
        let proto = match field "proto" with Some (`Int n) -> n | _ -> 1 in
        if proto <> Daemon.protocol then exit 0;
        Some (field "quiet" = Some (`Bool true))
  in
  let started seq =
    match first_look () with
    | None -> failed := true
    | Some quiet -> (
        match !w with
        | Some t when seq >= Wait.cursor t -> ()
        (* no cursor file yet, or the service's events have started over *)
        | _ ->
            let t = Wait.start ~cursor:seq ~quiet in
            w := Some t;
            keep_cursor t)
  in
  let rec again () =
    failed := false;
    ignore (look ());
    (match (!w, cursor_file ()) with None, Some (c, quiet) -> w := Some (Wait.start ~cursor:c ~quiet) | _ -> ());
    let after = Option.map Wait.cursor !w in
    (match Client.watch ~home ?after ~timeout:limit ~started ~idle:look feed with
    (* a service from another build refuses the watch itself, whatever its status would say *)
    | Error e when e = Daemon.other_build -> exit 0
    | _ -> ());
    Option.iter Wait.dropped !w;
    ignore (look ());
    nap ();
    again ()
  in
  again ()

(* [moth wait ...]: usage errors and anything unexpected exit 1, never 2 *)
let wait_command args =
  match Client.parse_args ("wait" :: args) with
  | Ok { opts; _ } -> (
      match List.assoc_opt "session" opts with
      | Some dir -> ( try wait ~session:dir with e -> fail 1 ("moth wait: " ^ Printexc.to_string e))
      | None -> fail 1 "wait is run by the agent's hook: moth wait --session <dir>")
  | Error m -> fail 1 m

let not_installed = "claude code is not installed, or not on PATH. moth works without it: use the window, or type moth commands here."

let absolute p = if Filename.is_relative p then Filename.concat (Sys.getcwd ()) p else p

let executable_file p =
  try
    let st = Unix.stat p in
    st.Unix.st_kind = Unix.S_REG && (try Unix.access p [ Unix.X_OK ]; true with Unix.Unix_error _ -> false)
  with Unix.Unix_error _ -> false

(* MOTH_CLAUDE when set, else claude in a PATH folder; an absolute path, since the agent starts in another folder *)
let find_claude () =
  match Sys.getenv_opt "MOTH_CLAUDE" with
  | Some p when p <> "" -> if executable_file p then Some (absolute p) else None
  | _ ->
      let dirs = String.split_on_char ':' (Option.value (Sys.getenv_opt "PATH") ~default:"") in
      List.find_map
        (fun d ->
          let f = Filename.concat (if d = "" then "." else d) "claude" in
          if executable_file f then Some (absolute f) else None)
        dirs

(* the newest event's seq, from the reply to a watch, which ends there *)
let newest_seq ~home =
  let exception Seq of int in
  match Client.watch ~home ~started:(fun seq -> raise (Seq seq)) ~idle:(fun () -> false) (fun _ -> false) with
  | exception Seq seq -> Ok seq
  | Ok () -> Error (Client.lost_contact home)
  | Error m -> Error m

(* [moth agent]: not a request. Writes the session's files and becomes Claude Code. The agent's
   own shell (MOTH_ACTOR=agent) may not start another. *)
let agent args =
  if Sys.getenv_opt "MOTH_ACTOR" = Some "agent" then fail 1 "this is already the agent's session";
  if args <> [] then fail 2 "agent takes no arguments";
  match find_claude () with
  | None -> fail 1 not_installed
  | Some claude -> (
      let home = Client.home () in
      (* the fields of status --json *)
      let status ~record =
        match Client.call ~record ~home ~actor:"user" ~cmd:"status" ~args:[] ~opts:[] () with
        | Error m -> fail 1 m
        | Ok (`Assoc fields, _) -> fields
        | Ok _ -> []
      in
      let proto = match List.assoc_opt "proto" (status ~record:true) with Some (`Int n) -> n | _ -> 1 in
      if proto <> Daemon.protocol then fail 1 "moth's background service is from another build of moth. stop it and it starts again by itself: moth stop";
      (* the session's cursor starts here, so the first waiter tells everything the person does
         from now on, however long the agent takes to load and start its first turn. Its quiet
         state is read after the seq: a quiet change after the seq is replayed by the waiter, and
         one before it is in this status *)
      let seq = match newest_seq ~home with Ok s -> s | Error m -> fail 1 m in
      let quiet = List.assoc_opt "quiet" (status ~record:false) = Some (`Bool true) in
      let moth = absolute Sys.executable_name in
      let path = Option.value (Sys.getenv_opt "PATH") ~default:"" in
      match Agent.prepare ~home ~moth ~path ~pid:(Unix.getpid ()) ~cursor:(seq, quiet) with
      | Error m -> fail 1 m
      | Ok dir -> (
          let give_up m =
            Agent.remove_session dir;
            fail 1 m
          in
          (try Unix.chdir (Filename.concat (Filename.dirname dir) "work")
           with Unix.Unix_error (e, _, _) -> give_up ("cannot start in moth's agent folder: " ^ Unix.error_message e));
          List.iter (fun (k, v) -> Unix.putenv k v) (Agent.env ~home ~moth ~path);
          try Unix.execv claude (Agent.command ~claude ~dir) with Unix.Unix_error _ -> give_up not_installed))

let () =
  match List.tl (Array.to_list Sys.argv) with
  | [] | [ "help" ] | [ "--help" ] | [ "-h" ] -> print_string usage
  | [ "daemon" ] -> (
      let home = Client.home () in
      try Daemon.run ~home ~start_helper:(Daemon.default_start_helper ~home) ~launch:(Daemon.default_launch ~home) with Failure m -> fail 1 m)
  | "wait" :: args -> wait_command args
  | "agent" :: args -> agent args
  | argv -> (
      let actor = match Client.actor_of_env (Sys.getenv_opt "MOTH_ACTOR") with Ok a -> a | Error m -> fail 2 m in
      match Client.parse_args argv with
      | Error m -> fail 2 m
      | Ok { cmd = "help"; args = []; _ } -> print_string usage
      | Ok { cmd = ("help" | "daemon") as cmd; _ } -> fail 2 (cmd ^ " takes no arguments")
      | Ok inv -> (
          match Daemon.check_usage ~cmd:inv.cmd ~args:inv.args ~opts:inv.opts with
          | Error m -> fail 2 m
          | Ok () when inv.cmd = "watch" -> watch ~json:inv.json ~after:(Option.map int_of_string (List.assoc_opt "after" inv.opts))
          | Ok () -> (
              let print result text = print_endline (if inv.json then Yojson.Safe.to_string result else text) in
              (* stopping must not start a daemon just to stop it *)
              match Client.call ~autostart:(inv.cmd <> "stop") ~home:(Client.home ()) ~actor ~cmd:inv.cmd ~args:inv.args ~opts:inv.opts () with
              | Ok (result, text) -> print result text
              | Error m when inv.cmd = "stop" && m = Client.not_running -> print (`Assoc [ ("message", `String m) ]) m
              | Error m -> fail 1 m)))
