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
         "hooks":{"Stop":[{"hooks":[{"type":"command","command":"'/opt/my moth/bin/moth' wait --session '/tmp/it'\\''s a home/agent/123'","asyncRewake":true,"timeout":86400}]}]}}|}
  in
  check_str "settings" (Yojson.Safe.to_string expected) (Yojson.Safe.to_string (Agent.settings ~home ~moth ~path ~dir))

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
      (* a dead pid: a child that has been reaped *)
      let dead =
        match Unix.fork () with
        | 0 -> Unix._exit 0
        | p ->
            ignore (Unix.waitpid [] p);
            string_of_int p
      in
      Unix.mkdir home 0o700;
      Unix.mkdir agent 0o700;
      List.iter (fun n -> Unix.mkdir (Filename.concat agent n) 0o700) [ dead; "notapid"; "0"; "work" ];
      write (Filename.concat (Filename.concat agent dead) "cursor") "1";
      write (Filename.concat (Filename.concat agent "work") "keep.txt") "x";
      let pid = Unix.getpid () + 1 in
      (match Agent.prepare ~home ~moth:"/opt/moth/bin/moth" ~path:"/usr/bin" ~pid with
      | Error m -> Alcotest.failf "prepare: %s" m
      | Ok dir ->
          check_str "the folder" (Filename.concat agent (string_of_int pid)) dir;
          Alcotest.(check int) "dir mode" 0o700 (mode dir);
          Alcotest.(check int) "agent mode" 0o700 (mode agent);
          Alcotest.(check int) "work mode" 0o700 (mode (Filename.concat agent "work"));
          Alcotest.(check int) "settings mode" 0o600 (mode (Filename.concat dir "settings.json"));
          Alcotest.(check int) "instructions mode" 0o600 (mode (Filename.concat dir "instructions.md"));
          check_str "settings parse and equal"
            (Yojson.Safe.to_string (Agent.settings ~home ~moth:"/opt/moth/bin/moth" ~path:"/usr/bin" ~dir))
            (Yojson.Safe.to_string (Yojson.Safe.from_file (Filename.concat dir "settings.json")));
          check_str "instructions" Agent.instructions (read (Filename.concat dir "instructions.md")));
      check_bool "dead sibling removed" false (Sys.file_exists (Filename.concat agent dead));
      List.iter (fun n -> check_bool (n ^ " kept") true (Sys.file_exists (Filename.concat agent n))) [ "notapid"; "0"; "work" ];
      check_bool "work content kept" true (Sys.file_exists (Filename.concat (Filename.concat agent "work") "keep.txt")))

let prepare_keeps_live () =
  with_tmp (fun d ->
      let live = string_of_int (Unix.getpid ()) in
      let agent = Filename.concat d "agent" in
      Unix.mkdir agent 0o700;
      Unix.mkdir (Filename.concat agent live) 0o700;
      (match Agent.prepare ~home:d ~moth:"/m/moth" ~path:"" ~pid:(Unix.getpid () + 1) with Ok _ -> () | Error m -> Alcotest.failf "prepare: %s" m);
      check_bool "live kept" true (Sys.file_exists (Filename.concat agent live)))

let instructions_say () =
  let has s = Alcotest.(check bool) s true (contains Agent.instructions s) in
  has "single-player and offline play only";
  has "never ask for one";
  has "--at"

let command_shape () =
  Alcotest.(check (array string))
    "command"
    [| "/c/claude"; "--settings"; "/d/settings.json"; "--append-system-prompt"; Agent.instructions; Agent.first_prompt |]
    (Agent.command ~claude:"/c/claude" ~dir:"/d")

(* ---- the real moth agent, with a stand-in for claude ---- *)

let run ?(env = []) ~home ~tmphome args =
  let err = Filename.concat tmphome "stderr.txt" and out = Filename.concat tmphome "stdout.txt" in
  let null = Unix.openfile "/dev/null" [ Unix.O_RDONLY ] 0 in
  let o = Unix.openfile out [ Unix.O_WRONLY; Unix.O_CREAT; Unix.O_TRUNC ] 0o600 in
  let e = Unix.openfile err [ Unix.O_WRONLY; Unix.O_CREAT; Unix.O_TRUNC ] 0o600 in
  let exe = Lazy.force moth_exe in
  let drop k = not (List.exists (fun p -> String.starts_with ~prefix:(p ^ "=") k) [ "MOTH_HOME"; "HOME"; "MOTH_CLAUDE"; "MOTH_ACTOR"; "PATH" ]) in
  let base = List.filter drop (Array.to_list (Unix.environment ())) in
  let env = Array.of_list (("MOTH_HOME=" ^ home) :: ("HOME=" ^ tmphome) :: (List.map (fun (k, v) -> k ^ "=" ^ v) env @ base)) in
  let pid = Unix.create_process_env exe (Array.of_list (exe :: args)) env null o e in
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
          let code, _, err, pid = run ~env:[ ("MOTH_CLAUDE", stub); ("PATH", "/usr/bin:/bin") ] ~home ~tmphome:tmp [ "agent" ] in
          Alcotest.(check int) ("exit 0 (" ^ err ^ ")") 0 code;
          let dir = Filename.concat (Filename.concat home "agent") (string_of_int pid) in
          check_bool "session folder is the stub's pid" true (Sys.file_exists (Filename.concat dir "settings.json"));
          let expect = Array.to_list (Agent.command ~claude:stub ~dir) |> List.tl in
          let got = read out in
          let want = String.concat "\n" (List.map (fun a -> "[" ^ a ^ "]") expect) ^ "\n" in
          check_bool "the arguments are command's" true (String.starts_with ~prefix:want got);
          let field k = List.find_map (fun l -> if String.starts_with ~prefix:(k ^ "=") l then Some (String.sub l (String.length k + 1) (String.length l - String.length k - 1)) else None) (String.split_on_char '\n' got) in
          let real p = try Unix.realpath p with _ -> p in
          check_str "directory" (real (Filename.concat (Filename.concat home "agent") "work")) (Option.get (field "pwd"));
          check_str "actor" "agent" (Option.get (field "actor"));
          check_str "home" home (Option.get (field "home"));
          check_str "pid" (string_of_int pid) (Option.get (field "pid"));
          let mothdir = Filename.dirname (Lazy.force moth_exe) in
          check_bool "path starts with moth's folder" true (String.starts_with ~prefix:(real mothdir ^ ":") (Option.get (field "path")) || String.starts_with ~prefix:(mothdir ^ ":") (Option.get (field "path")));
          (* nothing under HOME but the moth home and what the test itself wrote *)
          let names = List.sort compare (List.filter (fun n -> not (List.mem n [ "stub claude"; "stub.out"; "stderr.txt"; "stdout.txt" ])) (Array.to_list (Sys.readdir tmp))) in
          Alcotest.(check (list string)) "HOME holds only the moth home" [ "moth home" ] names))

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
          Alcotest.test_case "hook command" `Quick hook_command_runs;
          Alcotest.test_case "prepare" `Quick prepare_makes_files;
          Alcotest.test_case "prepare keeps a live session" `Quick prepare_keeps_live;
          Alcotest.test_case "instructions" `Quick instructions_say;
          Alcotest.test_case "command" `Quick command_shape;
        ] );
      ( "moth agent",
        [
          Alcotest.test_case "stand-in agent" `Quick agent_runs_stub;
          Alcotest.test_case "not installed" `Quick not_installed_cases;
          Alcotest.test_case "no arguments" `Quick takes_no_arguments;
        ] );
    ]
