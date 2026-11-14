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

(* through a temporary name, so a reader never sees half a number *)
let write_file path text =
  let tmp = path ^ ".tmp" in
  try
    let oc = open_out_bin tmp in
    Fun.protect ~finally:(fun () -> close_out_noerr oc) (fun () -> output_string oc text);
    Sys.rename tmp path
  with Sys_error _ -> ()

let process_alive pid = match Unix.kill pid 0 with () -> true | exception Unix.Unix_error (Unix.EPERM, _, _) -> true | exception Unix.Unix_error _ -> false

(* [moth wait --session <dir>], run by the agent's Stop hook. It follows the feed, and when the
   person has done something (see Wait) prints the note to stderr and exits 2, which wakes the
   agent. Every other end is exit 0 with nothing printed: the session is gone (its pid, the
   folder's name, is not alive, or the folder is), a newer waiter took over [dir]/waiter, or the
   service is from another build. It never starts a service: with none answering it asks again
   every second. *)
let wait ~session =
  let home = Client.home () in
  let dir =
    let rec strip d = if String.length d > 1 && d.[String.length d - 1] = '/' then strip (String.sub d 0 (String.length d - 1)) else d in
    strip session
  in
  let in_dir f = Filename.concat dir f in
  let session_pid = int_of_string_opt (Filename.basename dir) in
  let session_alive () = Sys.file_exists dir && Sys.is_directory dir && match session_pid with Some p -> process_alive p | None -> false in
  let me = string_of_int (Unix.getpid ()) in
  if not (session_alive ()) then exit 0;
  write_file (in_dir "waiter") me;
  let mine () = read_file (in_dir "waiter") = Some me in
  let cursor_file () = Option.bind (read_file (in_dir "cursor")) int_of_string_opt in
  let written = ref (cursor_file ()) in
  let keep_cursor n =
    if !written <> Some n then begin
      write_file (in_dir "cursor") (string_of_int n);
      written := Some n
    end
  in
  let w = ref None in
  (* what ended the watch: to tell, or to leave *)
  let ended = ref None in
  let look () =
    if not (session_alive () && mine ()) then begin
      ended := Some None;
      false
    end
    else
      match !w with
      | None -> true
      | Some t -> (
          match Wait.check t ~now:(Unix.gettimeofday ()) with
          | Wait.Keep_waiting ->
              if Wait.quiet t then keep_cursor (Wait.cursor t);
              true
          | Wait.Tell { notes; cursor } ->
              ended := Some (if mine () then Some (notes, cursor) else None);
              false)
  in
  let feed line =
    Option.iter (fun t -> Wait.line t ~now:(Unix.gettimeofday ()) line) !w;
    look ()
  in
  let rec again () =
    if not (session_alive () && mine ()) then exit 0;
    match Client.call ~autostart:false ~record:false ~home ~actor:"user" ~cmd:"status" ~args:[] ~opts:[] () with
    | Error _ ->
        Unix.sleepf 1.;
        again ()
    | Ok (status, _) ->
        let open Yojson.Safe.Util in
        let proto = match member "proto" status with `Int n -> n | _ -> 1 in
        if proto <> Daemon.protocol then exit 0;
        let quiet = member "quiet" status = `Bool true in
        (match (!w, cursor_file ()) with None, Some c -> w := Some (Wait.start ~cursor:c ~quiet) | _ -> ());
        let started seq =
          match !w with
          | Some t when seq >= Wait.cursor t -> ()
          (* the service's events start over: what was seen is no longer the newest *)
          | _ -> w := Some (Wait.start ~cursor:seq ~quiet)
        in
        let after = Option.map Wait.cursor !w in
        ignore (Client.watch ~home ?after ~started ~idle:look feed);
        (match !ended with
        | Some (Some (notes, cursor)) ->
            write_file (in_dir "cursor") (string_of_int cursor);
            prerr_endline notes;
            exit 2
        | Some None -> exit 0
        | None -> ());
        Option.iter Wait.dropped !w;
        Unix.sleepf 1.;
        again ()
  in
  again ()

let () =
  match List.tl (Array.to_list Sys.argv) with
  | [] | [ "help" ] | [ "--help" ] | [ "-h" ] -> print_string usage
  | [ "daemon" ] -> (
      let home = Client.home () in
      try Daemon.run ~home ~start_helper:(Daemon.default_start_helper ~home) ~launch:(Daemon.default_launch ~home) with Failure m -> fail 1 m)
  | argv -> (
      let actor = match Client.actor_of_env (Sys.getenv_opt "MOTH_ACTOR") with Ok a -> a | Error m -> fail 2 m in
      match Client.parse_args argv with
      | Error m -> fail 2 m
      | Ok { cmd = "help"; args = []; _ } -> print_string usage
      | Ok { cmd = ("help" | "daemon") as cmd; _ } -> fail 2 (cmd ^ " takes no arguments")
      | Ok { cmd = "wait"; opts; _ } -> (
          match List.assoc_opt "session" opts with
          | Some dir -> wait ~session:dir
          | None -> fail 2 "wait is run by the agent's hook: moth wait --session <dir>")
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
