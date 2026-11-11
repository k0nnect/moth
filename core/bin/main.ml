open Moth_core

let usage =
  {|usage: moth <command> [--json]

  bottles                              list crossover bottles
  ps --bottle <bottle>                 list the windows programs running in a bottle
  attach --bottle <bottle> <game.exe>  attach to a running game, by name or pid
  status                               what moth is attached to, and the hunt so far
  find <number>                        start a hunt for a number the game shows
  next <number>                        narrow the hunt to the number it shows now
  next down|up|same|changed            narrow the hunt by how the number moved
  results [--all]                      list the matches left
  set <#n> <number>                    write a number into result #n
  undo                                 put back what the last set replaced
  detach                               let go of the game
  log [-n <count>]                     recent commands, newest first
  stop                                 stop moth's background service
  daemon                               run the background service in the foreground

  --json prints the result as json.
|}

let fail code m =
  prerr_endline m;
  exit code

let () =
  match List.tl (Array.to_list Sys.argv) with
  | [] | [ "help" ] | [ "--help" ] | [ "-h" ] -> print_string usage
  | [ "daemon" ] -> (
      let home = Client.home () in
      try Daemon.run ~home ~start_helper:(Daemon.default_start_helper ~home) with Failure m -> fail 1 m)
  | argv -> (
      match Client.parse_args argv with
      | Error m -> fail 2 m
      | Ok { cmd = "help"; args = []; _ } -> print_string usage
      | Ok { cmd = ("help" | "daemon") as cmd; _ } -> fail 2 (cmd ^ " takes no arguments")
      | Ok inv -> (
          match Daemon.check_usage ~cmd:inv.cmd ~args:inv.args ~opts:inv.opts with
          | Error m -> fail 2 m
          | Ok () -> (
              let print result text = print_endline (if inv.json then Yojson.Safe.to_string result else text) in
              (* stopping must not start a daemon just to stop it *)
              match Client.call ~autostart:(inv.cmd <> "stop") ~home:(Client.home ()) ~actor:"user" ~cmd:inv.cmd ~args:inv.args ~opts:inv.opts () with
              | Ok (result, text) -> print result text
              | Error m when inv.cmd = "stop" && m = Client.not_running -> print (`Assoc [ ("message", `String m) ]) m
              | Error m -> fail 1 m)))
