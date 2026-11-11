(* The daemon runs on a thread with a temporary home and a fake helper; the client talks to it
   over the real socket. Nothing here starts wine. *)
open Moth_core
open Helper_api

let check_str n w g = Alcotest.(check string) n w g
let check_bool n w g = Alcotest.(check bool) n w g
let check_int n w g = Alcotest.(check int) n w g
let check_strs n w g = Alcotest.(check (list string)) n w g

let rec rm p =
  match Unix.lstat p with
  | exception Unix.Unix_error _ -> ()
  | { st_kind = S_DIR; _ } ->
      Array.iter (fun f -> rm (Filename.concat p f)) (Sys.readdir p);
      Unix.rmdir p
  | _ -> Sys.remove p

(* under /tmp rather than $TMPDIR: macOS caps unix socket paths at 104 bytes *)
let homes = ref 0

let fresh_home () =
  incr homes;
  let h = Printf.sprintf "/tmp/moth-test-%d-%d" (Unix.getpid ()) !homes in
  rm h;
  h

let sock home = Filename.concat home "core.sock"

(* ---- the fake helper the daemon starts: the same scripted helper for every bottle ---- *)

type fake = {
  mutable procs : (int * string) list;
  mutable modules : string list;
  mutable started : string list; (* bottles, newest first *)
  mutable closed : int;
  script : Fake_helper.script;
}

let fake ?(procs = [ (208, "disco.exe"); (300, "explorer.exe") ]) () =
  let h, script = Fake_helper.create () in
  let fk = { procs; modules = [ "UnityPlayer.dll" ]; started = []; closed = 0; script } in
  let helper =
    {
      h with
      ps = (fun () -> Ok fk.procs);
      modules = (fun _ -> Ok fk.modules);
      attach = (fun pid -> Ok { pid; name = List.assoc pid fk.procs; bits = 64 });
      close = (fun () -> fk.closed <- fk.closed + 1);
    }
  in
  let start_helper ~bottle =
    fk.started <- bottle :: fk.started;
    Ok helper
  in
  (fk, start_helper)

let gi ?(tracked = true) id count : group_info = { id; count; tracked; has_values = tracked }

(* ---- running a daemon ---- *)

type daemon = { home : string; thread : Thread.t }

let rec wait_up ?(tries = 500) path =
  let s = Unix.socket Unix.PF_UNIX Unix.SOCK_STREAM 0 in
  match Unix.connect s (Unix.ADDR_UNIX path) with
  | () -> Unix.close s
  | exception Unix.Unix_error _ ->
      Unix.close s;
      if tries = 0 then Alcotest.fail "the daemon did not come up";
      Thread.delay 0.01;
      wait_up ~tries:(tries - 1) path

let start ?(home = fresh_home ()) start_helper =
  let thread = Thread.create (fun () -> Daemon.run ~home ~start_helper) () in
  wait_up (sock home);
  { home; thread }

let call ?(actor = "user") ?(opts = []) d cmd args = Client.call ~autostart:false ~home:d.home ~actor ~cmd ~args ~opts ()
let text = function Ok (_, t) -> t | Error e -> Alcotest.failf "unexpected error: %s" e
let json = function Ok (j, _) -> j | Error e -> Alcotest.failf "unexpected error: %s" e
let check_err n msg = function Error e -> check_str n msg e | Ok (_, t) -> Alcotest.failf "%s: expected the error %S, got %S" n msg t
let check_json n w g = check_str n (Yojson.Safe.to_string w) (Yojson.Safe.to_string g)

let with_daemon ?home start_helper f =
  let d = start ?home start_helper in
  Fun.protect
    ~finally:(fun () ->
      ignore (call d "stop" []);
      Thread.join d.thread;
      rm d.home)
    (fun () -> f d)

let not_attached = "not attached. run: moth attach --bottle <bottle> <game.exe>"
let attached = "attached to disco.exe (pid 208, 64-bit) in uhmm"
let attach ?(bottle = "uhmm") ?(what = "disco.exe") d = call d "attach" [ what ] ~opts:[ ("bottle", bottle) ]

(* ---- tests ---- *)

let before_attach () =
  let _, sh = fake () in
  with_daemon sh (fun d ->
      check_err "before attach" not_attached (call d "find" [ "100" ]);
      List.iter (fun (cmd, args) -> check_err cmd not_attached (call d cmd args)) [ ("next", [ "85" ]); ("results", []); ("set", [ "1"; "5" ]); ("undo", []) ];
      check_str "status" "not attached" (text (call d "status" []));
      check_json "status json" (`Assoc [ ("attached", `Null); ("hunt", `Null) ]) (json (call d "status" []));
      check_str "detach" "not attached" (text (call d "detach" [])))

let attaching () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      check_str "attach" attached (text (attach d ~what:"DISCO.EXE"));
      check_json "attach json" (`Assoc [ ("message", `String attached) ]) (json (attach d));
      check_strs "one helper, in the bottle" [ "uhmm" ] fk.started;
      check_str "status" attached (text (call d "status" []));
      check_json "status json"
        (`Assoc [ ("attached", `Assoc [ ("pid", `Int 208); ("name", `String "disco.exe"); ("bits", `Int 64); ("bottle", `String "uhmm") ]); ("hunt", `Null) ])
        (json (call d "status" []));
      check_err "not running" "nope.exe isn't running in uhmm. start the game first." (attach d ~what:"nope.exe");
      check_err "no such pid" "pid 999 isn't running in uhmm." (attach d ~what:"999");
      check_str "still attached" attached (text (call d "status" []));
      check_str "by pid" "attached to explorer.exe (pid 300, 64-bit) in uhmm" (text (attach d ~what:"300"));
      fk.procs <- [ (209, "Disco.exe"); (208, "disco.exe") ];
      check_err "two" "more than one Disco.exe is running (pids 208, 209). attach by pid." (attach d);
      check_err "needs a bottle" "attach needs a bottle and a process: moth attach --bottle <bottle> <game.exe>" (call d "attach" [ "disco.exe" ]);
      check_err "needs a process" "attach needs a bottle and a process: moth attach --bottle <bottle> <game.exe>" (call d "attach" [] ~opts:[ ("bottle", "uhmm") ]);
      check_strs "the helper was reused" [ "uhmm" ] fk.started;
      check_int "and kept" 0 fk.closed;
      fk.procs <- [ (208, "disco.exe") ];
      check_str "another bottle" "attached to disco.exe (pid 208, 64-bit) in Disco Elysium" (text (attach d ~bottle:"Disco Elysium"));
      check_strs "a helper there" [ "Disco Elysium"; "uhmm" ] fk.started;
      check_int "the old one closed" 1 fk.closed;
      check_str "detach" "detached from disco.exe" (text (call d "detach" []));
      check_int "detach closes the helper" 2 fk.closed;
      check_str "status after detach" "not attached" (text (call d "status" [])))

let anticheat () =
  let fk, sh = fake ~procs:[ (208, "game.exe"); (412, "EasyAntiCheat_EOS.exe") ] () in
  with_daemon sh (fun d ->
      let refusal = "moth won't attach: easy anti-cheat is running with this game. moth is for single-player, offline play." in
      check_err "anticheat" refusal (attach d ~what:"game.exe");
      check_int "helper closed" 1 fk.closed;
      check_str "status" "not attached" (text (call d "status" []));
      fk.procs <- [ (208, "game.exe") ];
      check_str "attach" "attached to game.exe (pid 208, 64-bit) in uhmm" (text (attach d ~what:"game.exe"));
      fk.modules <- [ "kernel32.dll"; "EasyAntiCheat_x64.dll" ];
      check_err "in the modules" refusal (attach d ~what:"game.exe");
      check_str "detached" "not attached" (text (call d "status" []));
      check_int "closed again" 2 fk.closed)

let outcome_json ?(notes = []) ?(lost = 0) ?(ms = 0.) ?(bytes = 0) ?(wide = 0) ~main by_tag groups =
  `Assoc
    [
      ("main", `Int main);
      ("wide", `Int wide);
      ("by_tag", `List (List.map (fun (t, c) -> `List [ `String t; `Int c ]) by_tag));
      ("notes", `List (List.map (fun n -> `String n) notes));
      ("lost", `Int lost);
      ("dead_end", `Bool false);
      ("ms", `Float ms);
      ("bytes", `Int bytes);
      ("groups", `List groups);
    ]

let group_json ?(tier = "main") id count =
  `Assoc [ ("id", `String id); ("count", `Int count); ("tracked", `Bool true); ("has_values", `Bool true); ("tier", `String tier) ]

let hunt () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_scan f [ gi "i32" 3 ];
      check_str "find" "3 matches (3 whole number)" (text (call d "find" [ "100" ]));
      Fake_helper.queue_scan f ~stats:{ Fake_helper.zero_stats with bytes = 4096; ms = 2.5 } [ gi "i32" 3; gi "u8" 7 ];
      let j = json (call d "find" [ "100" ]) in
      check_bool "json" true (Yojson.Safe.Util.(member "main" j |> to_int) = 3);
      check_json "find json"
        (outcome_json ~main:3 ~wide:7 ~ms:2.5 ~bytes:4096 [ ("whole number", 3) ] [ group_json "i32" 3; group_json ~tier:"wide" "u8" 7 ])
        j;
      check_str "status with a hunt" (attached ^ "\n3 matches (3 whole number)") (text (call d "status" []));
      check_json "status json hunt"
        (`Assoc [ ("main", `Int 3); ("wide", `Int 7); ("groups", `List [ group_json "i32" 3; group_json ~tier:"wide" "u8" 7 ]) ])
        Yojson.Safe.Util.(member "hunt" (json (call d "status" [])));
      check_err "usage" "next needs a number, or one of: down, up, same, changed" (call d "next" []);
      check_err "bad number" "'abc' isn't a number" (call d "next" [ "abc" ]);
      (* no exact match: falls back to the direction, and the game freed two *)
      Fake_helper.queue_narrow f ~applied:false [];
      Fake_helper.queue_narrow f ~applied:true ~stats:{ Fake_helper.zero_stats with lost = 2 } [ gi "i32" 2; gi "u8" 1 ];
      let j = json (call d "next" [ "60" ]) in
      check_json "next json"
        (`List [ `String "fell_back_down"; `String "lost" ])
        Yojson.Safe.Util.(member "notes" j);
      check_int "lost" 2 Yojson.Safe.Util.(member "lost" j |> to_int);
      Fake_helper.queue_narrow f ~applied:true [ gi "i32" 1; gi "u8" 1 ];
      check_str "next by relation" "1 match (1 whole number)" (text (call d "next" [ "SAME" ]));
      check_bool "same" true (Fake_helper.last_narrow_pred f "i32" = Eq_prev);
      Fake_helper.queue_peek f "i32" [ { addr = 0x10008L; v = Vint 60L } ];
      check_str "results" "#1  60  0x10008  whole number" (text (call d "results" []));
      Fake_helper.queue_peek f "i32" [ { addr = 0x10008L; v = Vint 60L } ];
      Fake_helper.queue_peek f "u8" [ { addr = 0x2000L; v = Vint 60L } ];
      check_json "results json, all"
        (`List
          [
            `Assoc [ ("n", `Int 1); ("addr", `String "0x10008"); ("interp_id", `String "i32"); ("tag", `String "whole number"); ("shown", `String "60") ];
            `Assoc [ ("n", `Int 2); ("addr", `String "0x2000"); ("interp_id", `String "u8"); ("tag", `String "whole number (1-byte)"); ("shown", `String "60") ];
          ])
        (json (call d "results" [] ~opts:[ ("all", "true") ]));
      Fake_helper.queue_write f ~prev:(Vint 60L);
      check_bool "set accepts #" true (Result.is_ok (call d "set" [ "#1"; "999" ]));
      check_bool "wrote" true (Fake_helper.last_write f = (0x10008L, "i32", Vint 999L));
      Fake_helper.queue_write f ~prev:(Vint 999L);
      check_json "set without #" (`Assoc [ ("message", `String "set #2 to 5 (was 999)") ]) (json (call d "set" [ "2"; "5" ]));
      Fake_helper.queue_write f ~prev:(Vint 5L);
      check_str "undo" "restored #2 to 999" (text (call d "undo" []));
      check_err "set usage" "set needs a result number and a number: moth set <#n> <number>" (call d "set" [ "x"; "5" ]);
      check_err "set usage, one argument" "set needs a result number and a number: moth set <#n> <number>" (call d "set" [ "1" ]);
      check_err "find usage" "find needs a number" (call d "find" []);
      check_err "no such row" "no result #7. run: moth results" (call d "set" [ "7"; "1" ]))

let helper_lost () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_error fk.script "scan" { code = "helper_lost"; message = "" };
      check_err "lost" "lost contact with moth's helper. attach again." (call d "find" [ "100" ]);
      check_int "helper closed" 1 fk.closed;
      check_str "status" "not attached" (text (call d "status" []));
      ignore (text (attach d));
      check_strs "a new helper" [ "uhmm"; "uhmm" ] fk.started)

let target_gone () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_error fk.script "scan" { code = "target_gone"; message = "" };
      check_err "gone" "the game closed. attach again once it's running." (call d "find" [ "100" ]);
      check_str "status" "not attached" (text (call d "status" []));
      check_int "helper kept" 0 fk.closed;
      ignore (text (attach d));
      check_strs "helper reused" [ "uhmm" ] fk.started)

let ps () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      check_err "needs a bottle" "ps needs a bottle: moth ps --bottle <bottle>" (call d "ps" []);
      check_str "ps" "208  disco.exe\n300  explorer.exe" (text (call d "ps" [] ~opts:[ ("bottle", "uhmm") ]));
      check_json "ps json"
        (`List [ `Assoc [ ("pid", `Int 208); ("name", `String "disco.exe") ]; `Assoc [ ("pid", `Int 300); ("name", `String "explorer.exe") ] ])
        (json (call d "ps" [] ~opts:[ ("bottle", "uhmm") ]));
      ignore (text (attach d));
      check_strs "one helper for ps and attach" [ "uhmm" ] fk.started;
      fk.procs <- [ (8, "a.exe"); (1024, "b.exe") ];
      check_str "another bottle" "   8  a.exe\n1024  b.exe" (text (call d "ps" [] ~opts:[ ("bottle", "Disco Elysium") ]));
      check_strs "a helper for that bottle" [ "Disco Elysium"; "uhmm" ] fk.started;
      check_int "closed after ps" 1 fk.closed;
      check_str "still attached" attached (text (call d "status" [])))

let with_env k v f =
  let old = Sys.getenv_opt k in
  Unix.putenv k v;
  Fun.protect ~finally:(fun () -> Unix.putenv k (Option.value old ~default:"")) f

let bottles () =
  let dir = fresh_home () in
  Unix.mkdir dir 0o700;
  List.iter
    (fun b ->
      Unix.mkdir (Filename.concat dir b) 0o700;
      close_out (open_out (Filename.concat (Filename.concat dir b) "cxbottle.conf")))
    [ "uhmm"; "Disco Elysium" ];
  let _, sh = fake () in
  Fun.protect
    ~finally:(fun () -> rm dir)
    (fun () ->
      with_env "MOTH_BOTTLES" dir (fun () ->
          with_daemon sh (fun d ->
              check_str "bottles" "Disco Elysium\nuhmm" (text (call d "bottles" []));
              check_json "bottles json" (`List [ `String "Disco Elysium"; `String "uhmm" ]) (json (call d "bottles" []));
              check_err "takes nothing" "bottles takes no arguments" (call d "bottles" [ "x" ]))))

let events () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_scan fk.script [ gi "i32" 3 ];
      ignore (text (call d "find" [ "100" ]));
      ignore (call d "find" []);
      ignore (text (call ~actor:"agent" d "status" []));
      let ev = json (call d "log" [] ~opts:[ ("n", "2") ]) in
      let open Yojson.Safe.Util in
      check_int "two" 2 (List.length (to_list ev));
      check_str "actor" "agent" (index 0 ev |> member "actor" |> to_string);
      check_str "newest first" "status" (index 0 ev |> member "cmd" |> to_string);
      check_str "summary is the first line" attached (index 0 ev |> member "summary" |> to_string);
      check_strs "fields" [ "seq"; "ts"; "actor"; "cmd"; "args"; "ok"; "summary" ] (keys (index 0 ev));
      check_int "seq" 4 (index 0 ev |> member "seq" |> to_int);
      check_bool "ts" true (index 0 ev |> member "ts" |> to_number > 1e9);
      check_bool "error logged" false (index 1 ev |> member "ok" |> to_bool);
      check_str "error summary" "find needs a number" (index 1 ev |> member "summary" |> to_string);
      check_int "earlier one" 3 (index 1 ev |> member "seq" |> to_int);
      let all = json (call d "log" []) in
      check_int "the log call itself is logged" 5 (List.length (to_list all));
      check_strs "args" [ "100" ] (index 3 all |> member "args" |> to_list |> List.map to_string);
      let lines = String.split_on_char '\n' (text (call d "log" [] ~opts:[ ("n", "4") ])) in
      check_int "a line each" 4 (List.length lines);
      let ends_with suffix s = String.ends_with ~suffix s in
      check_bool "seq first" true (String.starts_with ~prefix:"4  " (List.nth lines 2));
      check_bool "text line" true (ends_with ("  agent  status  " ^ attached) (List.nth lines 2));
      check_bool "error line" true (ends_with "  user  find  error: find needs a number" (List.nth lines 3));
      let seven = String.split_on_char '\n' (text (call d "log" [] ~opts:[ ("n", "7") ])) in
      check_bool "args shown" true (ends_with "  user  find 100  3 matches (3 whole number)" (List.nth seven 5));
      check_err "n" "log -n needs a whole number" (call d "log" [] ~opts:[ ("n", "x") ]);
      let file = Filename.concat d.home "events.jsonl" in
      let ic = open_in file in
      let rec count n = match input_line ic with _ -> count (n + 1) | exception End_of_file -> n in
      let n = count 0 in
      close_in ic;
      check_int "one line per request" 9 n;
      check_int "events file mode" 0o600 (Unix.stat file).st_perm)

let stop_and_restart () =
  let fk, sh = fake () in
  let d = start sh in
  ignore (text (attach d));
  check_str "stop" "stopped" (text (call d "stop" []));
  check_bool "socket gone" false (Sys.file_exists (sock d.home));
  check_int "helper closed" 1 fk.closed;
  Thread.join d.thread;
  check_err "not running" "moth isn't running" (call d "status" []);
  (* a new daemon in the same home carries the event numbers on *)
  with_daemon ~home:d.home sh (fun d ->
      let ev = json (call d "log" [] ~opts:[ ("n", "1") ]) in
      let open Yojson.Safe.Util in
      check_str "last event" "stop" (index 0 ev |> member "cmd" |> to_string);
      check_int "its seq" 2 (index 0 ev |> member "seq" |> to_int);
      check_int "next seq" 3 (index 0 (json (call d "log" [] ~opts:[ ("n", "1") ])) |> member "seq" |> to_int))

let second_daemon () =
  let _, sh = fake () in
  with_daemon sh (fun d ->
      (match Daemon.run ~home:d.home ~start_helper:sh with
      | () -> Alcotest.fail "a second daemon ran"
      | exception Failure m -> check_str "second daemon refuses" "moth is already running" m);
      check_str "the first still serves" "not attached" (text (call d "status" [])))

let modes () =
  let _, sh = fake () in
  with_daemon sh (fun d ->
      check_int "home" 0o700 (Unix.stat d.home).st_perm;
      check_int "socket" 0o600 (Unix.stat (sock d.home)).st_perm)

let long_home () =
  let home = "/tmp/" ^ String.make 100 'm' in
  let msg = Printf.sprintf "the path %s/core.sock is too long for a unix socket. set MOTH_HOME to a shorter directory." home in
  check_err "client" msg (Client.call ~home ~actor:"user" ~cmd:"status" ~args:[] ~opts:[] ());
  let _, sh = fake () in
  (match Daemon.run ~home ~start_helper:sh with () -> Alcotest.fail "the daemon ran" | exception Failure m -> check_str "daemon" msg m);
  check_bool "nothing created" false (Sys.file_exists home);
  let home = "/dev/null/moth" in
  let msg = "couldn't create /dev/null/moth (Not a directory). set MOTH_HOME to a directory you can write to." in
  check_err "home can't be made" msg (Client.call ~home ~actor:"user" ~cmd:"status" ~args:[] ~opts:[] ());
  match Daemon.run ~home ~start_helper:sh with () -> Alcotest.fail "the daemon ran" | exception Failure m -> check_str "daemon, home can't be made" msg m

let autostart () =
  let _, sh = fake () in
  let home = fresh_home () in
  Unix.mkdir home 0o700;
  (* a stale socket: a plain file at the socket's path, no daemon *)
  close_out (open_out (sock home));
  let thread = ref None in
  let spawn_daemon ~home = thread := Some (Thread.create (fun () -> Daemon.run ~home ~start_helper:sh) ()) in
  Fun.protect
    ~finally:(fun () -> rm home)
    (fun () ->
      check_str "autostart cleans stale socket" "not attached" (text (Client.call ~spawn_daemon ~home ~actor:"user" ~cmd:"status" ~args:[] ~opts:[] ()));
      check_bool "spawned" true (!thread <> None);
      check_str "stop" "stopped" (text (Client.call ~autostart:false ~home ~actor:"user" ~cmd:"stop" ~args:[] ~opts:[] ()));
      Option.iter Thread.join !thread)

let autostart_gives_up () =
  let home = fresh_home () in
  Fun.protect
    ~finally:(fun () -> rm home)
    (fun () ->
      check_err "no autostart" "moth isn't running" (Client.call ~autostart:false ~home ~actor:"user" ~cmd:"status" ~args:[] ~opts:[] ());
      let t0 = Unix.gettimeofday () in
      check_err "gives up"
        (Printf.sprintf "couldn't start moth's background service. see %s/core.log" home)
        (Client.call ~spawn_daemon:(fun ~home:_ -> ()) ~home ~actor:"user" ~cmd:"status" ~args:[] ~opts:[] ());
      check_bool "after about 5 seconds" true (Unix.gettimeofday () -. t0 >= 4.9);
      check_int "home made private" 0o700 (Unix.stat home).st_perm)

let signals () =
  List.iter
    (fun signal ->
      let fk, sh = fake () in
      let d = start sh in
      ignore (text (attach d));
      (* block the signal here so the daemon's thread is the one it reaches *)
      let old = Thread.sigmask Unix.SIG_BLOCK [ signal ] in
      Unix.kill (Unix.getpid ()) signal;
      Thread.join d.thread;
      ignore (Thread.sigmask Unix.SIG_SETMASK old);
      check_bool "socket gone" false (Sys.file_exists (sock d.home));
      check_int "helper closed" 1 fk.closed;
      rm d.home)
    [ Sys.sigterm; Sys.sigint ]

let unexpected () =
  let _, sh = fake () in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      (* nothing queued: the fake helper raises *)
      (match call d "find" [ "100" ] with
      | Error e -> check_bool "reported" true (String.starts_with ~prefix:"moth hit an internal error: " e)
      | Ok _ -> Alcotest.fail "expected an error");
      check_str "still serving" attached (text (call d "status" [])))

let usage () =
  let check n w g = Alcotest.(check (result unit string)) n w g in
  check "unknown" (Error "unknown command: frob. run: moth help") (Daemon.check_usage ~cmd:"frob" ~args:[] ~opts:[]);
  check "find" (Ok ()) (Daemon.check_usage ~cmd:"find" ~args:[ "1" ] ~opts:[]);
  check "next" (Error "next needs a number, or one of: down, up, same, changed") (Daemon.check_usage ~cmd:"next" ~args:[] ~opts:[]);
  check "set" (Ok ()) (Daemon.check_usage ~cmd:"set" ~args:[ "#3"; "-5" ] ~opts:[]);
  check "log" (Ok ()) (Daemon.check_usage ~cmd:"log" ~args:[] ~opts:[ ("n", "50") ]);
  let _, sh = fake () in
  with_daemon sh (fun d -> check_err "from the daemon" "unknown command: frob. run: moth help" (call d "frob" []))

let parse_args () =
  let p = Client.parse_args in
  let inv cmd args opts json : (Client.invocation, string) result = Ok { cmd; args; opts; json } in
  let same n w g = check_bool n true (w = g) in
  same "plain" (inv "find" [ "100" ] [] false) (p [ "find"; "100" ]);
  same "negative number" (inv "next" [ "-5" ] [] false) (p [ "next"; "-5" ]);
  same "decimal" (inv "find" [ "-.5" ] [] false) (p [ "find"; "-.5" ]);
  same "options" (inv "attach" [ "disco.exe" ] [ ("bottle", "Disco Elysium") ] true) (p [ "attach"; "--bottle"; "Disco Elysium"; "disco.exe"; "--json" ]);
  same "all and n" (inv "log" [] [ ("all", "true"); ("n", "5") ] false) (p [ "log"; "--all"; "-n"; "5" ]);
  same "options first" (inv "status" [] [] true) (p [ "--json"; "status" ]);
  same "last one wins" (inv "ps" [] [ ("bottle", "b") ] false) (p [ "ps"; "--bottle"; "a"; "--bottle"; "b" ]);
  same "unknown option" (Error "unknown option: --frob. run: moth help") (p [ "status"; "--frob" ]);
  same "missing value" (Error "--bottle needs a value") (p [ "ps"; "--bottle" ]);
  same "no command" (Error "no command. run: moth help") (p [ "--json" ])

let home () =
  with_env "MOTH_HOME" "/x/y" (fun () -> check_str "env" "/x/y" (Client.home ()));
  with_env "MOTH_HOME" "rel" (fun () -> check_str "made absolute" (Filename.concat (Sys.getcwd ()) "rel") (Client.home ()));
  with_env "MOTH_HOME" "" (fun () ->
      with_env "HOME" "/Users/u" (fun () -> check_str "default" "/Users/u/Library/Application Support/moth" (Client.home ())))

let () =
  let t n f = Alcotest.test_case n `Quick f in
  Alcotest.run "daemon"
    [
      ( "commands",
        [
          t "before attach" before_attach;
          t "attach" attaching;
          t "anti-cheat" anticheat;
          t "the hunt" hunt;
          t "helper lost" helper_lost;
          t "target gone" target_gone;
          t "ps" ps;
          t "bottles" bottles;
          t "events" events;
          t "usage" usage;
          t "unexpected error" unexpected;
        ] );
      ( "lifecycle",
        [
          t "stop and restart" stop_and_restart;
          t "second daemon" second_daemon;
          t "modes" modes;
          t "long or unwritable home" long_home;
          t "autostart" autostart;
          t "autostart gives up" autostart_gives_up;
          t "signals" signals;
        ] );
      ("client", [ t "parse args" parse_args; t "home" home ]);
    ]
