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
  stop                                 stop moth's background service
  daemon                               run the background service in the foreground

  a result number can be written 3 or #3. in a script, write 3 or quote '#3'.
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
   (it fell behind), it watches again from the newest event it printed. *)
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
    match Client.watch ~home ?after:!newest show with
    | Ok () -> follow ~first:false
    | Error m when m = Client.not_running && not first ->
        print_endline (if json then Yojson.Safe.to_string (`Assoc [ ("message", `String "moth stopped") ]) else "moth stopped")
    | Error m -> fail 1 m
  in
  (* stdout closed (moth watch | head) ends it quietly *)
  try follow ~first:true with Sys_error _ -> ()

let () =
  match List.tl (Array.to_list Sys.argv) with
  | [] | [ "help" ] | [ "--help" ] | [ "-h" ] -> print_string usage
  | [ "daemon" ] -> (
      let home = Client.home () in
      try Daemon.run ~home ~start_helper:(Daemon.default_start_helper ~home) ~launch:(Daemon.default_launch ~home) with Failure m -> fail 1 m)
  | argv -> (
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
              match Client.call ~autostart:(inv.cmd <> "stop") ~home:(Client.home ()) ~actor:"user" ~cmd:inv.cmd ~args:inv.args ~opts:inv.opts () with
              | Ok (result, text) -> print result text
              | Error m when inv.cmd = "stop" && m = Client.not_running -> print (`Assoc [ ("message", `String m) ]) m
              | Error m -> fail 1 m)))
