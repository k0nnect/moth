open Moth_core

let check_str = Alcotest.(check string)
let check_bool = Alcotest.(check bool)

let rec rm p =
  match Unix.lstat p with
  | exception Unix.Unix_error _ -> ()
  | { st_kind = S_DIR; _ } ->
      Array.iter (fun f -> rm (Filename.concat p f)) (Sys.readdir p);
      Unix.rmdir p
  | _ -> Sys.remove p

(* under /tmp rather than $TMPDIR: macOS caps unix socket paths at 104 bytes; the name has a space
   and a single quote in it *)
let count = ref 0

let with_tmp f =
  incr count;
  let d = Printf.sprintf "/tmp/moth ag-%d-%d's" (Unix.getpid ()) !count in
  rm d;
  Unix.mkdir d 0o700;
  Fun.protect ~finally:(fun () -> rm d) (fun () -> f d)

let contains h n =
  let hl = String.length h and nl = String.length n in
  let rec go i = i + nl <= hl && (String.sub h i nl = n || go (i + 1)) in
  go 0

let read path = try In_channel.with_open_bin path In_channel.input_all with Sys_error _ -> ""

let write ?(perm = 0o644) path text =
  Out_channel.with_open_bin path (fun oc -> output_string oc text);
  Unix.chmod path perm

let mode p = (Unix.stat p).Unix.st_perm land 0o777
let moth_exe = lazy (Filename.concat (Sys.getcwd ()) "../bin/main.exe")

(* ---- settings and prepare ---- *)

let settings_exact () =
  let home = "/tmp/it's a home" and moth = "/opt/my moth/bin/moth" and path = "/usr/bin:/bin" and dir = "/tmp/it's a home/agent/123" in
  let expected =
    Yojson.Safe.from_string
      {|{"env":{"MOTH_ACTOR":"agent","MOTH_HOME":"/tmp/it's a home","PATH":"/opt/my moth/bin:/usr/bin:/bin"},
         "permissions":{"allow":["Bash(moth *)"]},
         "hooks":{"UserPromptSubmit":[{"hooks":[{"type":"command","command":"'/opt/my moth/bin/moth' wait --session '/tmp/it'\\''s a home/agent/123'","asyncRewake":true,"timeout":86400}]}],
                  "Stop":[{"hooks":[{"type":"command","command":"'/opt/my moth/bin/moth' wait --session '/tmp/it'\\''s a home/agent/123'","asyncRewake":true,"timeout":86400}]}]}}|}
  in
  check_str "settings" (Yojson.Safe.to_string expected) (Yojson.Safe.to_string (Agent.settings ~home ~moth ~path ~dir))

(* a turn's start and its end arm the same waiter: one command, so the newest waiter takes over *)
let both_hooks_one_command () =
  let s = Agent.settings ~home:"/h" ~moth:"/m/moth" ~path:"/usr/bin" ~dir:"/h/agent/7" in
  let command event = Yojson.Safe.Util.(s |> member "hooks" |> member event |> index 0 |> member "hooks" |> index 0 |> member "command" |> to_string) in
  check_str "the same command" (command "Stop") (command "UserPromptSubmit");
  Alcotest.(check (list string)) "the two events, in this order" [ "UserPromptSubmit"; "Stop" ] Yojson.Safe.Util.(s |> member "hooks" |> keys)

let hook_command_runs () =
  with_tmp (fun d ->
      let bin = Filename.concat d "bin dir's" in
      Unix.mkdir bin 0o700;
      let moth = Filename.concat bin "moth" in
      write ~perm:0o755 moth "#!/bin/sh\nfor a in \"$@\"; do printf '%s\\n' \"$a\"; done\n";
      let dir = Filename.concat d "agent/9 9" in
      let s = Agent.settings ~home:d ~moth ~path:"/usr/bin" ~dir in
      let cmd = Yojson.Safe.Util.(s |> member "hooks" |> member "Stop" |> index 0 |> member "hooks" |> index 0 |> member "command" |> to_string) in
      let out = Filename.concat d "out" in
      let code = Sys.command (Printf.sprintf "sh -c %s > %s" (Filename.quote cmd) (Filename.quote out)) in
      Alcotest.(check int) "ran" 0 code;
      check_str "arguments whole" ("wait\n--session\n" ^ dir ^ "\n") (read out))

let prepare_makes_files () =
  with_tmp (fun d ->
      let home = Filename.concat d "home" in
      let agent = Filename.concat home "agent" in
      let moth = "/opt/moth/bin/moth" in
      match Agent.prepare ~home ~moth ~path:"/usr/bin" ~pid:(Unix.getpid ()) ~cursor:(7, true) with
      | Error m -> Alcotest.failf "prepare: %s" m
      | Ok dir ->
          check_str "the folder" (Filename.concat agent (string_of_int (Unix.getpid ()))) dir;
          Alcotest.(check int) "home mode" 0o700 (mode home);
          Alcotest.(check int) "agent mode" 0o700 (mode agent);
          Alcotest.(check int) "work mode" 0o700 (mode (Filename.concat agent "work"));
          Alcotest.(check int) "dir mode" 0o700 (mode dir);
          Alcotest.(check int) "settings mode" 0o600 (mode (Filename.concat dir "settings.json"));
          Alcotest.(check int) "instructions mode" 0o600 (mode (Filename.concat dir "instructions.md"));
          Alcotest.(check int) "cursor mode" 0o600 (mode (Filename.concat dir "cursor"));
          check_str "the cursor: the seq, and quiet" "7 quiet" (read (Filename.concat dir "cursor"));
          check_str "settings parse and equal"
            (Yojson.Safe.to_string (Agent.settings ~home ~moth ~path:"/usr/bin" ~dir))
            (Yojson.Safe.to_string (Yojson.Safe.from_file (Filename.concat dir "settings.json")));
          check_str "instructions" Agent.instructions (read (Filename.concat dir "instructions.md")))

(* a child that has been reaped: its pid is not alive, and never the test's own *)
let dead_pid () =
  match Unix.fork () with
  | 0 -> Unix._exit 0
  | p ->
      ignore (Unix.waitpid [] p);
      string_of_int p

let prepare_removes_dead () =
  with_tmp (fun d ->
      let agent = Filename.concat d "agent" in
      let dead = dead_pid () in
      Unix.mkdir agent 0o700;
      List.iter (fun n -> Unix.mkdir (Filename.concat agent n) 0o700) [ dead; "notapid"; "0"; "work" ];
      write (Filename.concat (Filename.concat agent dead) "cursor") "1";
      write (Filename.concat (Filename.concat agent "work") "keep.txt") "x";
      (match Agent.prepare ~home:d ~moth:"/m/moth" ~path:"/usr/bin" ~pid:(Unix.getpid ()) ~cursor:(0, false) with Ok _ -> () | Error m -> Alcotest.failf "prepare: %s" m);
      check_bool "dead sibling removed" false (Sys.file_exists (Filename.concat agent dead));
      List.iter (fun n -> check_bool (n ^ " kept") true (Sys.file_exists (Filename.concat agent n))) [ "notapid"; "0"; "work" ];
      check_bool "work content kept" true (Sys.file_exists (Filename.concat (Filename.concat agent "work") "keep.txt")))

let prepare_replaces_stale_own () =
  with_tmp (fun d ->
      let pid = Unix.getpid () in
      let dir = Filename.concat (Filename.concat d "agent") (string_of_int pid) in
      Unix.mkdir (Filename.concat d "agent") 0o700;
      Unix.mkdir dir 0o755;
      write (Filename.concat dir "cursor") "7 quiet";
      write (Filename.concat dir "waiter") "1";
      (match Agent.prepare ~home:d ~moth:"/m/moth" ~path:"/usr/bin" ~pid ~cursor:(3, false) with Ok _ -> () | Error m -> Alcotest.failf "prepare: %s" m);
      Alcotest.(check (list string)) "only the three files" [ "cursor"; "instructions.md"; "settings.json" ] (List.sort compare (Array.to_list (Sys.readdir dir)));
      check_str "the new cursor, not the stale one" "3" (read (Filename.concat dir "cursor"));
      Alcotest.(check int) "mode" 0o700 (mode dir))

let prepare_keeps_live () =
  with_tmp (fun d ->
      let live = string_of_int (Unix.getpid ()) in
      let agent = Filename.concat d "agent" in
      Unix.mkdir agent 0o700;
      Unix.mkdir (Filename.concat agent live) 0o700;
      (match Agent.prepare ~home:d ~moth:"/m/moth" ~path:"" ~pid:(Unix.getpid () + 1) ~cursor:(0, false) with Ok _ -> () | Error m -> Alcotest.failf "prepare: %s" m);
      check_bool "live kept" true (Sys.file_exists (Filename.concat agent live)))

let instructions_say () =
  let has s = Alcotest.(check bool) s true (contains Agent.instructions s) in
  has "single-player and offline play only";
  has "never ask for one";
  has "moth refuses a row from you without --at";
  (* how a value is found, kept and changed, and what moth refuses from the agent: these lines, in
     this order, one after the other *)
  let lines = String.split_on_char '\n' Agent.instructions in
  let expected =
    [
      "- to find a value: moth find <the number on screen>, have them change it in the game, moth next <the new number>, until few are left. with no number to read: moth snapshot, then moth next down, up, same or changed. moth results lists what is left.";
      "- to keep or change a row, give its number bare (3, not #3: the shell reads # as a comment) and its address: moth keep 3 <name> --at <the address moth results showed>, moth set 3 <number> --at <that address>. moth refuses a row from you without --at. a row listed by moth results --near has no number of its own: keep it by address, moth keep <0xaddr> <the number it shows> <name>. a kept value is changed by name: moth set <name> <number>.";
      "- you cannot start a program the person has not started from moth before, and quiet is theirs to switch: moth refuses both from you.";
    ]
  in
  let rec from = function [] -> [] | l :: rest as all -> if l = List.hd expected then all else from rest in
  Alcotest.(check (list string)) "the three lines" expected (List.filteri (fun i _ -> i < 3) (from lines));
  Alcotest.(check bool) "no #n left" false (contains Agent.instructions "<#n>")

let command_shape () =
  Alcotest.(check (array string))
    "command"
    [| "/c/claude"; "--settings"; "/d/settings.json"; "--append-system-prompt"; Agent.instructions; Agent.first_prompt |]
    (Agent.command ~claude:"/c/claude" ~dir:"/d")

(* ---- the real moth agent, with a stand-in for claude ---- *)

let run ?(env = []) ?cwd ~home ~tmphome args =
  let err = Filename.concat tmphome "stderr.txt" and out = Filename.concat tmphome "stdout.txt" in
  let null = Unix.openfile "/dev/null" [ Unix.O_RDONLY ] 0 in
  let o = Unix.openfile out [ Unix.O_WRONLY; Unix.O_CREAT; Unix.O_TRUNC ] 0o600 in
  let e = Unix.openfile err [ Unix.O_WRONLY; Unix.O_CREAT; Unix.O_TRUNC ] 0o600 in
  let exe = Lazy.force moth_exe in
  let drop k = not (List.exists (fun p -> String.starts_with ~prefix:(p ^ "=") k) [ "MOTH_HOME"; "HOME"; "MOTH_CLAUDE"; "MOTH_ACTOR"; "PATH" ]) in
  let base = List.filter drop (Array.to_list (Unix.environment ())) in
  let env = Array.of_list (("MOTH_HOME=" ^ home) :: ("HOME=" ^ tmphome) :: (List.map (fun (k, v) -> k ^ "=" ^ v) env @ base)) in
  let pid =
    match cwd with
    | None -> Unix.create_process_env exe (Array.of_list (exe :: args)) env null o e
    | Some c ->
        let old = Sys.getcwd () in
        Sys.chdir c;
        Fun.protect ~finally:(fun () -> Sys.chdir old) (fun () -> Unix.create_process_env exe (Array.of_list (exe :: args)) env null o e)
  in
  List.iter Unix.close [ null; o; e ];
  let _, st = Unix.waitpid [] pid in
  ((match st with Unix.WEXITED n -> n | _ -> -1), String.trim (read out), String.trim (read err), pid)

let stop_service ~home ~tmphome =
  ignore (run ~home ~tmphome [ "stop" ]);
  (* the service removes its socket as it ends *)
  let deadline = Unix.gettimeofday () +. 5. in
  while Sys.file_exists (Filename.concat home "core.sock") && Unix.gettimeofday () < deadline do
    Unix.sleepf 0.05
  done;
  check_bool "no service left" false (Sys.file_exists (Filename.concat home "core.sock"))

let not_installed = "claude code is not installed, or not on PATH. moth works without it: use the window, or type moth commands here."

let agent_runs_stub () =
  with_tmp (fun tmp ->
      let home = Filename.concat tmp "moth home" in
      let stub = Filename.concat tmp "stub claude" in
      let out = Filename.concat tmp "stub.out" in
      write ~perm:0o755 stub
        (Printf.sprintf
           "#!/bin/sh\n{ for a in \"$@\"; do printf '[%%s]\\n' \"$a\"; done; printf 'pwd=%%s\\n' \"$(pwd -P)\"; printf 'actor=%%s\\nhome=%%s\\npath=%%s\\npid=%%s\\n' \"$MOTH_ACTOR\" \"$MOTH_HOME\" \"$PATH\" \"$$\"; } > %s\n"
           (Filename.quote out));
      Fun.protect
        ~finally:(fun () -> stop_service ~home ~tmphome:tmp)
        (fun () ->
          (* MOTH_HOME given relative: the agent must see the absolute one *)
          let code, _, err, pid = run ~cwd:tmp ~env:[ ("MOTH_CLAUDE", stub); ("PATH", "/usr/bin:/bin") ] ~home:"moth home" ~tmphome:tmp [ "agent" ] in
          Alcotest.(check int) ("exit 0 (" ^ err ^ ")") 0 code;
          let dir = Filename.concat (Filename.concat home "agent") (string_of_int pid) in
          check_bool "session folder is the stub's pid" true (Sys.file_exists (Filename.concat dir "settings.json"));
          (* the relative MOTH_HOME was made absolute in the agent's own (resolved) folder *)
          let rdir = Filename.concat (Filename.concat (Unix.realpath home) "agent") (string_of_int pid) in
          let expect = Array.to_list (Agent.command ~claude:stub ~dir:rdir) |> List.tl in
          let got = read out in
          let want = String.concat "\n" (List.map (fun a -> "[" ^ a ^ "]") expect) ^ "\n" in
          check_bool "the arguments are command's" true (String.starts_with ~prefix:want got);
          let field k = List.find_map (fun l -> if String.starts_with ~prefix:(k ^ "=") l then Some (String.sub l (String.length k + 1) (String.length l - String.length k - 1)) else None) (String.split_on_char '\n' got) in
          let real p = try Unix.realpath p with _ -> p in
          check_str "directory" (real (Filename.concat (Filename.concat home "agent") "work")) (Option.get (field "pwd"));
          check_str "actor" "agent" (Option.get (field "actor"));
          check_str "home" (Unix.realpath home) (Option.get (field "home"));
          check_str "pid" (string_of_int pid) (Option.get (field "pid"));
          let mothdir = Filename.dirname (Lazy.force moth_exe) in
          check_bool "path starts with moth's folder" true (String.starts_with ~prefix:(real mothdir ^ ":") (Option.get (field "path")) || String.starts_with ~prefix:(mothdir ^ ":") (Option.get (field "path")));
          (* the cursor is the service's newest seq (moth log's newest; log does not list itself),
             written before the agent started, so the first waiter tells all after it *)
          let _, newest, _, _ = run ~home ~tmphome:tmp [ "log"; "-n"; "1"; "--json" ] in
          let seq = Yojson.Safe.Util.(Yojson.Safe.from_string newest |> index 0 |> member "seq" |> to_int) in
          check_str "the cursor" (string_of_int seq) (read (Filename.concat dir "cursor"));
          Alcotest.(check int) "cursor mode" 0o600 (mode (Filename.concat dir "cursor"));
          (* nothing under HOME but the moth home and what the test itself wrote *)
          let names = List.sort compare (List.filter (fun n -> not (List.mem n [ "stub claude"; "stub.out"; "stderr.txt"; "stdout.txt" ])) (Array.to_list (Sys.readdir tmp))) in
          Alcotest.(check (list string)) "HOME holds only the moth home" [ "moth home" ] names))

(* with quiet on, the cursor says so: the first waiter starts quiet *)
let the_cursor_says_quiet () =
  with_tmp (fun tmp ->
      let home = Filename.concat tmp "h" in
      let stub = Filename.concat tmp "stub" in
      write ~perm:0o755 stub "#!/bin/sh\n";
      Fun.protect
        ~finally:(fun () -> stop_service ~home ~tmphome:tmp)
        (fun () ->
          ignore (run ~home ~tmphome:tmp [ "find"; "3" ]);
          ignore (run ~home ~tmphome:tmp [ "quiet"; "on" ]);
          let code, _, err, pid = run ~env:[ ("MOTH_CLAUDE", stub); ("PATH", "/usr/bin:/bin") ] ~home ~tmphome:tmp [ "agent" ] in
          Alcotest.(check int) ("exit 0 (" ^ err ^ ")") 0 code;
          (* find, quiet on, and moth agent's own status *)
          check_str "the cursor" "3 quiet" (read (Filename.concat (Filename.concat (Filename.concat home "agent") (string_of_int pid)) "cursor"))))

let not_installed_cases () =
  with_tmp (fun tmp ->
      let home = Filename.concat tmp "h" in
      let check name env =
        let code, _, err, _ = run ~env ~home ~tmphome:tmp [ "agent" ] in
        Alcotest.(check int) (name ^ " exit") 1 code;
        check_str (name ^ " text") not_installed err;
        check_bool (name ^ " no session") false (Sys.file_exists (Filename.concat home "agent"));
        check_bool (name ^ " no service") false (Sys.file_exists (Filename.concat home "core.sock"))
      in
      check "missing program" [ ("MOTH_CLAUDE", "/nonexistent"); ("PATH", "/usr/bin:/bin") ];
      let plain = Filename.concat tmp "plain" in
      write plain "x";
      check "not executable" [ ("MOTH_CLAUDE", plain); ("PATH", "/usr/bin:/bin") ];
      check "empty path" [ ("PATH", "") ])

let exec_fails () =
  with_tmp (fun tmp ->
      let home = Filename.concat tmp "h" in
      let bad = Filename.concat tmp "bad" in
      write ~perm:0o755 bad "#!/nonexistent/interpreter\n";
      Fun.protect
        ~finally:(fun () -> stop_service ~home ~tmphome:tmp)
        (fun () ->
          let code, _, err, pid = run ~env:[ ("MOTH_CLAUDE", bad); ("PATH", "/usr/bin:/bin") ] ~home ~tmphome:tmp [ "agent" ] in
          Alcotest.(check int) "exit" 1 code;
          check_str "text" not_installed err;
          check_bool "no session folder" false (Sys.file_exists (Filename.concat (Filename.concat home "agent") (string_of_int pid)))))

(* a listener on core.sock that answers one status with no proto, then goes *)
let other_build () =
  with_tmp (fun tmp ->
      let home = Filename.concat tmp "h" in
      Unix.mkdir home 0o700;
      let stub = Filename.concat tmp "stub" in
      let ran = Filename.concat tmp "ran" in
      write ~perm:0o755 stub (Printf.sprintf "#!/bin/sh\n: > %s\n" (Filename.quote ran));
      let listener = Unix.socket Unix.PF_UNIX Unix.SOCK_STREAM 0 in
      Unix.bind listener (Unix.ADDR_UNIX (Filename.concat home "core.sock"));
      Unix.listen listener 4;
      let th =
        Thread.create
          (fun () ->
            let c, _ = Unix.accept listener in
            let ic = Unix.in_channel_of_descr c and oc = Unix.out_channel_of_descr c in
            let line = input_line ic in
            let id = try Yojson.Safe.Util.(member "id" (Yojson.Safe.from_string line)) with _ -> `Null in
            output_string oc (Yojson.Safe.to_string (`Assoc [ ("id", id); ("ok", `Bool true); ("result", `Assoc [ ("attached", `Bool false) ]); ("text", `String "") ]) ^ "\n");
            flush oc;
            (try ignore (input_line ic) with End_of_file | Sys_error _ -> ());
            Unix.close c)
          ()
      in
      let code, _, err, _ = run ~env:[ ("MOTH_CLAUDE", stub); ("PATH", "/usr/bin:/bin") ] ~home ~tmphome:tmp [ "agent" ] in
      Thread.join th;
      Unix.close listener;
      Alcotest.(check int) "exit" 1 code;
      check_str "text" "moth's background service is from another build of moth. stop it and it starts again by itself: moth stop" err;
      check_bool "no session folder" false (Sys.file_exists (Filename.concat home "agent"));
      check_bool "stub not run" false (Sys.file_exists ran))

(* a service of the test's own on [home]/core.sock while [f] runs: each connection's one request
   goes to [answer], whose reply is sent back, and the connection is held until the client hangs
   up. Gives [f]'s result and the requests, in the order they came. *)
let with_fake_service ~home answer f =
  let path = Filename.concat home "core.sock" in
  let listener = Unix.socket Unix.PF_UNIX Unix.SOCK_STREAM 0 in
  Unix.bind listener (Unix.ADDR_UNIX path);
  Unix.listen listener 8;
  let seen = ref [] and over = ref false in
  let serve c =
    let ic = Unix.in_channel_of_descr c in
    (match input_line ic with
    | line ->
        let request = try Yojson.Safe.from_string line with Yojson.Json_error _ -> `Null in
        seen := request :: !seen;
        let reply = Yojson.Safe.to_string (answer request) ^ "\n" in
        ignore (Unix.write_substring c reply 0 (String.length reply));
        (try
           while true do
             ignore (input_line ic)
           done
         with End_of_file | Sys_error _ -> ())
    | exception (End_of_file | Sys_error _) -> ());
    close_in_noerr ic
  in
  let th =
    Thread.create
      (fun () ->
        while not !over do
          match Unix.select [ listener ] [] [] 0.05 with [], _, _ -> () | _ -> serve (fst (Unix.accept listener))
        done)
      ()
  in
  let result = Fun.protect ~finally:(fun () -> over := true; Thread.join th; Unix.close listener; try Sys.remove path with Sys_error _ -> ()) f in
  (result, List.rev !seen)

let id_of request = try Yojson.Safe.Util.member "id" request with Yojson.Safe.Util.Type_error _ -> `Null
let served request result = `Assoc [ ("id", id_of request); ("ok", `Bool true); ("result", result); ("text", `String "") ]
let refused request e = `Assoc [ ("id", id_of request); ("ok", `Bool false); ("error", `String e) ]

(* a request as [cmd], and [(unrecorded)] after it when it asked to leave no trace *)
let describe request =
  let open Yojson.Safe.Util in
  let cmd = try to_string (member "cmd" request) with Type_error _ -> "?" in
  if (try member "record" request with Type_error _ -> `Null) = `Bool false then cmd ^ " (unrecorded)" else cmd

let cmd_of request = try Yojson.Safe.Util.(to_string (member "cmd" request)) with Yojson.Safe.Util.Type_error _ -> ""

(* the cursor's seq comes from the watch, and its quiet state from a status read after it, so a
   quiet on between the first status and the watch is not missed *)
let quiet_read_after_the_seq () =
  with_tmp (fun tmp ->
      let home = Filename.concat tmp "h" in
      Unix.mkdir home 0o700;
      let stub = Filename.concat tmp "stub" in
      write ~perm:0o755 stub "#!/bin/sh\n";
      let statuses = ref 0 in
      let answer request =
        match cmd_of request with
        | "status" ->
            incr statuses;
            served request (`Assoc [ ("attached", `Bool false); ("proto", `Int Daemon.protocol); ("quiet", `Bool (!statuses > 1)) ])
        | "watch" -> served request (`Assoc [ ("seq", `Int 7) ])
        | _ -> refused request "not asked for in this test"
      in
      let (code, _, err, pid), seen = with_fake_service ~home answer (fun () -> run ~env:[ ("MOTH_CLAUDE", stub); ("PATH", "/usr/bin:/bin") ] ~home ~tmphome:tmp [ "agent" ]) in
      Alcotest.(check int) ("exit 0 (" ^ err ^ ")") 0 code;
      Alcotest.(check (list string)) "status, the watch, then status unrecorded" [ "status"; "watch"; "status (unrecorded)" ] (List.map describe seen);
      check_str "the cursor: the watch's seq, the second status's quiet" "7 quiet" (read (Filename.concat (Filename.concat (Filename.concat home "agent") (string_of_int pid)) "cursor")))

(* the agent's own shell cannot start a second agent: refused before anything is written *)
let not_from_the_agents_shell () =
  with_tmp (fun tmp ->
      let home = Filename.concat tmp "h" in
      let stub = Filename.concat tmp "stub" and ran = Filename.concat tmp "ran" in
      write ~perm:0o755 stub (Printf.sprintf "#!/bin/sh\n: > %s\n" (Filename.quote ran));
      let code, _, err, _ = run ~env:[ ("MOTH_CLAUDE", stub); ("MOTH_ACTOR", "agent"); ("PATH", "/usr/bin:/bin") ] ~home ~tmphome:tmp [ "agent" ] in
      Alcotest.(check int) "exit" 1 code;
      check_str "text" "this is already the agent's session" err;
      check_bool "no home made, so no service and no session folder" false (Sys.file_exists home);
      check_bool "stub not run" false (Sys.file_exists ran))

let takes_no_arguments () =
  with_tmp (fun tmp ->
      let code, _, err, _ = run ~home:(Filename.concat tmp "h") ~tmphome:tmp [ "agent"; "now" ] in
      Alcotest.(check int) "exit" 2 code;
      check_str "text" "agent takes no arguments" err)

let () =
  Alcotest.run "agent"
    [
      ( "files",
        [
          Alcotest.test_case "settings" `Quick settings_exact;
          Alcotest.test_case "both hooks run one command" `Quick both_hooks_one_command;
          Alcotest.test_case "hook command" `Quick hook_command_runs;
          Alcotest.test_case "prepare" `Quick prepare_makes_files;
          Alcotest.test_case "prepare removes dead sessions" `Quick prepare_removes_dead;
          Alcotest.test_case "prepare replaces a stale own folder" `Quick prepare_replaces_stale_own;
          Alcotest.test_case "prepare keeps a live session" `Quick prepare_keeps_live;
          Alcotest.test_case "instructions" `Quick instructions_say;
          Alcotest.test_case "command" `Quick command_shape;
        ] );
      ( "moth agent",
        [
          Alcotest.test_case "stand-in agent" `Quick agent_runs_stub;
          Alcotest.test_case "the cursor says quiet" `Quick the_cursor_says_quiet;
          Alcotest.test_case "not installed" `Quick not_installed_cases;
          Alcotest.test_case "no arguments" `Quick takes_no_arguments;
          Alcotest.test_case "program that cannot run" `Quick exec_fails;
          Alcotest.test_case "service from another build" `Quick other_build;
          Alcotest.test_case "quiet is read after the seq" `Quick quiet_read_after_the_seq;
          Alcotest.test_case "not from the agent's own shell" `Quick not_from_the_agents_shell;
        ] );
    ]
