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
  mutable modules : (string list, err) result;
  mutable started : string list; (* bottles, newest first *)
  mutable closed : int;
  mutable modules_asked : int; (* modules calls *)
  mutable attaches : int; (* attach calls *)
  mutable attached_to : attached option; (* what the helper last attached to *)
  mutable alive : bool; (* whether that process is still running *)
  mutable statuses : int; (* status calls *)
  mutable status_lost : bool; (* status answers helper_lost *)
  mutable detaches : int; (* detach calls *)
  mutable detach_fails : bool; (* detach answers an error *)
  script : Fake_helper.script;
}

let fake ?(procs = [ (208, "disco.exe"); (300, "explorer.exe") ]) () =
  let h, script = Fake_helper.create () in
  let fk =
    {
      procs;
      modules = Ok [ "UnityPlayer.dll" ];
      started = [];
      closed = 0;
      modules_asked = 0;
      attaches = 0;
      attached_to = None;
      alive = true;
      statuses = 0;
      status_lost = false;
      detaches = 0;
      detach_fails = false;
      script;
    }
  in
  let helper =
    {
      h with
      ps = (fun () -> Ok fk.procs);
      modules =
        (fun _ ->
          fk.modules_asked <- fk.modules_asked + 1;
          fk.modules);
      attach =
        (fun pid ->
          fk.attaches <- fk.attaches + 1;
          let a = { pid; name = List.assoc pid fk.procs; bits = 64 } in
          fk.attached_to <- Some a;
          fk.alive <- true;
          Ok a);
      (* a set's read-back answers read_failed unless a test queues one: held null, and a second
         line that moth couldn't read it back *)
      read =
        (fun ~addr ~vtype ->
          if Queue.is_empty script.reads then Fake_helper.queue_error script "read" { code = "read_failed"; message = "" };
          h.read ~addr ~vtype);
      (* the game it attached to, whatever is running now *)
      status =
        (fun () ->
          fk.statuses <- fk.statuses + 1;
          if fk.status_lost then Error { code = "helper_lost"; message = "" }
          else match fk.attached_to with Some a when fk.alive -> Ok (Some a) | _ -> Ok None);
      detach =
        (fun () ->
          fk.detaches <- fk.detaches + 1;
          if fk.detach_fails then Error { code = "not_attached"; message = "no process attached" } else Ok ());
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
      check_int "modules never asked for" 0 fk.modules_asked;
      check_int "never attached" 0 fk.attaches;
      check_str "status" "not attached" (text (call d "status" []));
      fk.procs <- [ (208, "game.exe") ];
      check_str "attach" "attached to game.exe (pid 208, 64-bit) in uhmm" (text (attach d ~what:"game.exe"));
      check_int "attached once" 1 fk.attaches;
      fk.modules <- Ok [ "kernel32.dll"; "EasyAntiCheat_x64.dll" ];
      fk.attaches <- 0;
      check_err "in the modules" refusal (attach d ~what:"game.exe");
      check_int "not attached for it" 0 fk.attaches;
      check_str "detached" "not attached" (text (call d "status" []));
      check_int "closed again" 2 fk.closed;
      (* modules that can't be listed refuse the attach instead of skipping the check *)
      fk.modules <- Error { code = "modules_failed"; message = "cannot list the modules of pid 208, error 5" };
      check_err "modules unknown" "couldn't check what's loaded in game.exe, so moth won't attach. try again." (attach d ~what:"game.exe");
      check_int "still not attached" 0 fk.attaches;
      check_str "status" "not attached" (text (call d "status" [])))

let outcome_json ?(notes = []) ?(lost = 0) ?(ms = 0.) ?(bytes = 0) ?(unreadable = 0) ?(wide = 0) ~main by_tag groups =
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
      ("unreadable", `Int unreadable);
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
      Fake_helper.queue_scan f ~stats:{ Fake_helper.zero_stats with bytes = 4096; unreadable_bytes = 8192; ms = 2.5 } [ gi "i32" 3; gi "u8" 7 ];
      let j = json (call d "find" [ "100" ]) in
      check_bool "json" true (Yojson.Safe.Util.(member "main" j |> to_int) = 3);
      check_json "find json"
        (outcome_json ~main:3 ~wide:7 ~ms:2.5 ~bytes:4096 ~unreadable:8192 [ ("whole number", 3) ] [ group_json "i32" 3; group_json ~tier:"wide" "u8" 7 ])
        j;
      Fake_helper.queue_scan f ~stats:{ Fake_helper.zero_stats with unreadable_bytes = 8192 } [ gi "i32" 3; gi "u8" 7 ];
      check_str "find text" "3 matches (3 whole number)\ncouldn't read 8 kb of the game's memory, so matches there are missing."
        (text (call d "find" [ "100" ]));
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
      Fake_helper.queue_peek f "i32" [ { addr = 0x10008L; v = Some (Vint 60L) } ];
      check_str "results" "#1  60  0x10008  whole number" (text (call d "results" []));
      Fake_helper.queue_peek f "i32" [ { addr = 0x10008L; v = Some (Vint 60L) } ];
      Fake_helper.queue_peek f "u8" [ { addr = 0x2000L; v = Some (Vint 60L) } ];
      check_json "results json, all"
        (`Assoc
          [
            ( "rows",
              `List
                [
                  `Assoc
                    [ ("n", `Int 1); ("addr", `String "0x10008"); ("interp_id", `String "i32"); ("tag", `String "whole number"); ("shown", `String "60"); ("away", `Null) ];
                  `Assoc
                    [
                      ("n", `Int 2); ("addr", `String "0x2000"); ("interp_id", `String "u8"); ("tag", `String "whole number (1-byte)"); ("shown", `String "60"); ("away", `Null);
                    ];
                ] );
            ("total", `Int 2);
            ("from", `Int 1);
          ])
        (json (call d "results" [] ~opts:[ ("all", "true") ]));
      Fake_helper.queue_write f ~prev:(Vint 60L);
      check_bool "set accepts #" true (Result.is_ok (call d "set" [ "#1"; "999" ]));
      check_bool "wrote" true (Fake_helper.last_write f = (0x10008L, "i32", Put_value (Vint 999L)));
      Fake_helper.queue_write f ~prev:(Vint 999L);
      Fake_helper.queue_read f (Vint 5L);
      check_json "set without #" (`Assoc [ ("message", `String "set #2 to 5 (was 999)"); ("held", `Bool true) ]) (json (call d "set" [ "2"; "5" ]));
      Fake_helper.queue_write f ~prev:(Vint 5L);
      check_str "undo" "restored #2 to 999" (text (call d "undo" []));
      let set_usage = "set needs a result number or a kept name, and a number: moth set <#n|name> <number>" in
      check_err "set usage" set_usage (call d "set" [ "#x"; "5" ]);
      check_err "set usage, one argument" set_usage (call d "set" [ "1" ]);
      check_err "not a result number, so a name" "no kept value called x. see: moth kept" (call d "set" [ "x"; "5" ]);
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

let write_record () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_scan f [ gi "i32" 1; gi "f32" 1 ];
      ignore (text (call d "find" [ "60" ]));
      Fake_helper.queue_peek f "i32" [ { addr = 0x10008L; v = Some (Vint 60L) } ];
      Fake_helper.queue_peek f "f32" [ { addr = 0x2000L; v = Some (Vfloat 60.) } ];
      ignore (text (call d "results" []));
      Fake_helper.queue_write f ~prev:(Vint 60L) ~prev_bits:0x3cL;
      ignore (text (call d "set" [ "1"; "999" ]));
      Fake_helper.queue_write f ~prev:(Vfloat 60.) ~prev_bits:0x42700000L;
      ignore (text (call d "set" [ "2"; "-1.5" ]));
      Fake_helper.queue_write f ~prev:(Vfloat (-1.5)) ~prev_bits:0xbfc00000L;
      ignore (text (call d "undo" []));
      check_err "a refused set" "this value is a whole number, and 1.5 has a decimal part" (call d "set" [ "1"; "1.5" ]);
      let open Yojson.Safe.Util in
      (* newest first: the refused set, undo, set, set, results *)
      let ev = json (call d "log" [] ~opts:[ ("n", "5") ]) in
      let write i = member "write" (index i ev) in
      let wrote addr vtype prev_bits new_bits =
        `Assoc [ ("addr", `String addr); ("type", `String vtype); ("prev_bits", `String prev_bits); ("new_bits", `String new_bits) ]
      in
      check_json "set, whole number" (wrote "0x10008" "i32" "0x0000003c" "0x000003e7") (write 3);
      check_json "set, decimal" (wrote "0x2000" "f32" "0x42700000" "0xbfc00000") (write 2);
      check_json "undo" (wrote "0x2000" "f32" "0xbfc00000" "0x42700000") (write 1);
      check_strs "fields" [ "seq"; "ts"; "actor"; "cmd"; "args"; "ok"; "summary"; "write" ] (keys (index 1 ev));
      check_bool "nothing written, no record" true (write 0 = `Null);
      check_bool "other commands, no record" true (write 4 = `Null))

(* attaching again to the same pid keeps the hunt only while the helper still has that game *)
let reattach () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_scan fk.script [ gi "i32" 3 ];
      ignore (text (call d "find" [ "100" ]));
      check_str "again" attached (text (attach d));
      check_int "not attached twice" 1 fk.attaches;
      check_str "the hunt goes on" (attached ^ "\n3 matches (3 whole number)") (text (call d "status" []));
      (* the game closed, and the one running now has the same pid *)
      fk.alive <- false;
      check_str "attached afresh" attached (text (attach d));
      check_int "attached again" 2 fk.attaches;
      check_str "a new hunt" attached (text (call d "status" [])))

let crossover_missing () =
  let bottles = fresh_home () in
  Unix.mkdir bottles 0o700;
  let home = fresh_home () in
  let missing = "crossover not found. set MOTH_CROSSOVER to the path of CrossOver.app." in
  Fun.protect
    ~finally:(fun () -> rm bottles)
    (fun () ->
      with_env "MOTH_CROSSOVER" "/nonexistent/CrossOver.app" (fun () ->
          with_env "MOTH_BOTTLES" bottles (fun () ->
              with_daemon ~home (Daemon.default_start_helper ~home) (fun d ->
                  check_err "attach" missing (attach d);
                  check_err "ps" missing (call d "ps" [] ~opts:[ ("bottle", "uhmm") ])))))

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

(* ---- stage 1b: pages, near, kept values, checked writes, a closed game, the budget ---- *)

(* [k] items of 3 from [base], 4 bytes apart *)
let threes ?(base = 0x1000) k = List.init k (fun i -> { addr = Int64.of_int (base + (4 * i)); v = Some (Vint 3L) })

let lines s = String.split_on_char '\n' s
let last_line s = List.nth (lines s) (List.length (lines s) - 1)
let name_msg = "a name is lowercase letters, digits, - and _, starting with a letter."
let nothing_kept = "nothing kept yet. keep a result with: moth keep <#n> [name]"

(* a hunt for 3 with [count] matches in one i32 group *)
let hunt_for_three d (f : Fake_helper.script) count =
  ignore (text (attach d));
  Fake_helper.queue_scan f [ gi "i32" count ];
  ignore (text (call d "find" [ "3" ]))

let pages () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      hunt_for_three d f 30;
      Fake_helper.queue_peek f "i32" (threes ~base:0x1050 5);
      let r = call d "results" [] ~opts:[ ("from", "21"); ("n", "5") ] in
      check_bool "from 21, 5 rows" true (Fake_helper.last_peek f "i32" = (20, 5));
      let open Yojson.Safe.Util in
      let j = json r in
      check_strs "fields" [ "rows"; "total"; "from" ] (keys j);
      check_int "total" 30 (member "total" j |> to_int);
      check_int "from" 21 (member "from" j |> to_int);
      check_json "a row"
        (`Assoc [ ("n", `Int 21); ("addr", `String "0x1050"); ("interp_id", `String "i32"); ("tag", `String "whole number"); ("shown", `String "3"); ("away", `Null) ])
        (index 0 (member "rows" j));
      Alcotest.(check (list int)) "numbered" [ 21; 22; 23; 24; 25 ] (List.map (fun r -> member "n" r |> to_int) (to_list (member "rows" j)));
      check_int "5 rows and a line" 6 (List.length (lines (text r)));
      check_str "first row" "#21  3  0x1050  whole number" (List.hd (lines (text r)));
      check_str "more" "showing 21-25 of 30. more: moth results --from 26" (last_line (text r));
      (* the last page has no such line *)
      Fake_helper.queue_peek f "i32" (threes ~base:0x1064 5);
      let t = text (call d "results" [] ~opts:[ ("from", "26"); ("n", "5") ]) in
      check_int "5 rows alone" 5 (List.length (lines t));
      check_str "the last row" "#30  3  0x1074  whole number" (last_line t);
      (* 20 by default; --all is kept for the next page *)
      Fake_helper.queue_peek f "i32" (threes 20);
      let t = text (call d "results" [] ~opts:[ ("all", "true") ]) in
      check_bool "20 rows" true (Fake_helper.last_peek f "i32" = (0, 20));
      check_str "more, all" "showing 1-20 of 30. more: moth results --all --from 21" (last_line t);
      check_err "past the end" "no result #31. there are 30." (call d "results" [] ~opts:[ ("from", "31") ]))

let near_pages () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      hunt_for_three d f 3;
      let near = [ { addr = 0x6c551060L; v = Some (Vint 3L) }; { addr = 0x6c551068L; v = Some (Vint 3L) }; { addr = 0x6c551078L; v = Some (Vint 3L) } ] in
      Fake_helper.queue_peek_from f "i32" near;
      let r = call d "results" [] ~opts:[ ("near", "0x6c551068"); ("n", "2") ] in
      check_str "nearest first, a page"
        "#1  3  0x6c551068  0   whole number\n#2  3  0x6c551060  -8  whole number\nshowing 1-2 of 3. more: moth results --from 3 --near 0x6c551068" (text r);
      let open Yojson.Safe.Util in
      Alcotest.(check (list (option int))) "away" [ Some 0; Some (-8) ] (List.map (fun r -> member "away" r |> to_int_option) (to_list (member "rows" (json r))));
      check_int "total" 3 (member "total" (json r) |> to_int);
      Fake_helper.queue_peek_from f "i32" near;
      check_str "the next page" "#3  3  0x6c551078  +16  whole number" (text (call d "results" [] ~opts:[ ("near", "0x6c551068"); ("from", "3") ]));
      check_err "a name not kept" "no kept value called xp. see: moth kept" (call d "results" [] ~opts:[ ("near", "xp") ]))

let results_usage () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      hunt_for_three d f 30;
      let calls = List.length (Fake_helper.calls f) in
      let n = "results -n needs a number from 1 to 200" and from = "results --from needs a row number, 1 or more" in
      List.iter
        (fun (opts, msg) -> check_err (String.concat " " (List.map (fun (k, v) -> k ^ "=" ^ v) opts)) msg (call d "results" [] ~opts))
        [
          ([ ("n", "0") ], n);
          ([ ("n", "201") ], n);
          ([ ("n", "99999") ], n);
          ([ ("n", "abc") ], n);
          ([ ("n", "-5") ], n);
          ([ ("n", "") ], n);
          ([ ("from", "0") ], from);
          ([ ("from", "x") ], from);
          ([ ("from", "-1") ], from);
          ([ ("n", "5"); ("from", "0") ], from);
        ];
      check_int "the helper was not asked" calls (List.length (Fake_helper.calls f));
      let ok n w g = Alcotest.(check (result unit string)) n w g in
      ok "1 and 200" (Ok ()) (Daemon.check_usage ~cmd:"results" ~args:[] ~opts:[ ("n", "200"); ("from", "1"); ("near", "xp") ]);
      ok "1" (Ok ()) (Daemon.check_usage ~cmd:"results" ~args:[] ~opts:[ ("n", "1") ]))

let keep_by_name () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_scan f [ gi "i32" 1 ];
      ignore (text (call d "find" [ "60" ]));
      Fake_helper.queue_peek f "i32" [ { addr = 0x10008L; v = Some (Vint 60L) } ];
      ignore (text (call d "results" []));
      let r = call d "keep" [ "#1"; "xp" ] in
      check_str "keep" "kept #1 as xp (60, whole number)" (text r);
      check_json "keep json" (`Assoc [ ("message", `String "kept #1 as xp (60, whole number)") ]) (json r);
      Fake_helper.queue_read f (Vint 60L);
      Fake_helper.queue_read f (Vint 60L);
      check_str "kept" "xp  60  0x10008  whole number" (text (call d "kept" []));
      check_json "kept json"
        (`List [ `Assoc [ ("name", `String "xp"); ("addr", `String "0x10008"); ("tag", `String "whole number"); ("shown", `String "60") ] ])
        (json (call d "kept" []));
      Fake_helper.queue_write f ~prev:(Vint 60L) ~prev_bits:0x3cL;
      Fake_helper.queue_read f (Vint 95L);
      check_json "set by name" (`Assoc [ ("message", `String "set xp to 95 (was 60)"); ("held", `Bool true) ]) (json (call d "set" [ "xp"; "95" ]));
      check_bool "written at xp" true (Fake_helper.last_write f = (0x10008L, "i32", Put_value (Vint 95L)));
      Fake_helper.queue_write f ~prev:(Vint 95L) ~prev_bits:0x5fL;
      check_str "undo" "restored xp to 60" (text (call d "undo" []));
      (* both events keep their write *)
      let open Yojson.Safe.Util in
      let ev = json (call d "log" [] ~opts:[ ("n", "2") ]) in
      let wrote addr prev_bits new_bits = `Assoc [ ("addr", `String addr); ("type", `String "i32"); ("prev_bits", `String prev_bits); ("new_bits", `String new_bits) ] in
      check_json "set's write" (wrote "0x10008" "0x0000003c" "0x0000005f") (member "write" (index 1 ev));
      check_json "undo's write" (wrote "0x10008" "0x0000005f" "0x0000003c") (member "write" (index 0 ev));
      let r = call d "forget" [ "xp" ] in
      check_str "forget" "forgot xp" (text r);
      check_json "forget json" (`Assoc [ ("message", `String "forgot xp") ]) (json r);
      check_str "nothing kept" nothing_kept (text (call d "kept" []));
      check_json "nothing kept, json" (`List []) (json (call d "kept" []));
      check_err "forgotten" "no kept value called xp. see: moth kept" (call d "forget" [ "xp" ]);
      check_err "set, forgotten" "no kept value called xp. see: moth kept" (call d "set" [ "xp"; "1" ]);
      check_err "set, a name there can't be" "no kept value called X Y. see: moth kept" (call d "set" [ "X Y"; "1" ]);
      check_err "set, an address"
        "no kept value called 0x6c551068. to change an address, keep it first: moth keep 0x6c551068 <the number it shows>"
        (call d "set" [ "0x6c551068"; "95" ]);
      check_err "forget, an address" "no kept value called 0x6c551068. see: moth kept" (call d "forget" [ "0x6c551068" ]);
      check_err "forget usage" "forget needs a name: moth forget <name>" (call d "forget" []);
      check_err "forget usage, two" "forget needs a name: moth forget <name>" (call d "forget" [ "a"; "b" ]);
      check_err "kept takes nothing" "kept takes no arguments" (call d "kept" [ "xp" ]))

let keep_by_address () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_read f (Vint 15L);
      check_str "keep at" "kept 0x6c551068 as xp (15, whole number)" (text (call d "keep" [ "0x6c551068"; "15"; "xp" ]));
      check_bool "read there, as i32" true (Fake_helper.reads f = [ (0x6c551068L, "i32") ]);
      Fake_helper.queue_read f (Vint 15L);
      check_str "no name, any case" "kept 0x6c551068 as v1 (15, whole number)" (text (call d "keep" [ "0X6C551068"; "15" ]));
      (* nothing queued: every read fails *)
      check_err "nothing there" "nothing at 0x6c551068 reads as 7." (call d "keep" [ "0x6c551068"; "7"; "lvl" ]);
      Fake_helper.queue_read f (Vint 15L);
      Fake_helper.queue_read f (Vint 15L);
      check_str "both" "xp  15  0x6c551068  whole number\nv1  15  0x6c551068  whole number" (text (call d "kept" []));
      (* nothing queued: neither can be read now *)
      check_str "unreadable" "xp  ?  0x6c551068  whole number\nv1  ?  0x6c551068  whole number" (text (call d "kept" []));
      Fake_helper.queue_read f (Vint 15L);
      check_json "unreadable json"
        (`List
          [
            `Assoc [ ("name", `String "xp"); ("addr", `String "0x6c551068"); ("tag", `String "whole number"); ("shown", `String "15") ];
            `Assoc [ ("name", `String "v1"); ("addr", `String "0x6c551068"); ("tag", `String "whole number"); ("shown", `String "?") ];
          ])
        (json (call d "kept" [])))

let keep_refused () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      hunt_for_three d f 2;
      Fake_helper.queue_peek f "i32" (threes 2);
      ignore (text (call d "results" []));
      check_err "a number for a name" name_msg (call d "keep" [ "#2"; "5" ]);
      check_err "an address for a name" name_msg (call d "keep" [ "#2"; "0x10" ]);
      let usage = "keep needs a result, or an address and the number it shows: moth keep <#n> [name], or moth keep <0xaddr> <number> [name]" in
      List.iter
        (fun args -> check_err (String.concat " " ("keep" :: args)) usage (call d "keep" args))
        [ []; [ "0x10" ]; [ "#1"; "a"; "b" ]; [ "#0" ]; [ "#x" ]; [ "0x"; "5" ]; [ "0xg1"; "5" ]; [ "0x10"; "5"; "a"; "b" ] ];
      check_str "nothing kept" nothing_kept (text (call d "kept" [])))

(* R16: keep takes a bare result number, as set does, so a script need not quote # *)
let keep_a_bare_number () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      hunt_for_three d f 2;
      Fake_helper.queue_peek f "i32" (threes 2);
      ignore (text (call d "results" []));
      check_str "keep 1 xp" "kept #1 as xp (3, whole number)" (text (call d "keep" [ "1"; "xp" ]));
      check_str "keep 1" "kept #1 as v1 (3, whole number)" (text (call d "keep" [ "1" ]));
      check_err "keep 2 5: the name rule" name_msg (call d "keep" [ "2"; "5" ]);
      check_err "a row not listed" "no result #15. run: moth results" (call d "keep" [ "15" ]);
      let usage = "keep needs a result, or an address and the number it shows: moth keep <#n> [name], or moth keep <0xaddr> <number> [name]" in
      List.iter (fun args -> check_err (String.concat " " ("keep" :: args)) usage (call d "keep" args)) [ [ "0" ]; [ "x" ]; [ "0"; "xp" ]; [ "x"; "xp" ]; [ "1"; "a"; "b" ] ];
      Fake_helper.queue_read f (Vint 3L);
      Fake_helper.queue_read f (Vint 3L);
      check_str "kept, and nothing else" "xp  3  0x1000  whole number\nv1  3  0x1000  whole number" (text (call d "kept" [])))

let set_put_back () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_scan f [ gi "i32" 1 ];
      ignore (text (call d "find" [ "6" ]));
      Fake_helper.queue_peek f "i32" [ { addr = 0x10008L; v = Some (Vint 6L) } ];
      ignore (text (call d "results" []));
      Fake_helper.queue_write f ~prev:(Vint 6L) ~prev_bits:6L;
      Fake_helper.queue_read f (Vint 6L);
      let r = call d "set" [ "#1"; "9" ] in
      let two = "set #1 to 9 (was 6)\nthe game put it straight back to 6. it's worked out from something else, so writing here won't stick." in
      check_str "two lines" two (text r);
      check_json "held false" (`Assoc [ ("message", `String two); ("held", `Bool false) ]) (json r))

(* R15: the read-back lost the game or the helper. The write landed, so the reply is the set's,
   with held null and a second line; its event keeps the write; and the next command says not
   attached *)
let a_write_that_could_not_be_checked () =
  List.iter
    (fun (code, why) ->
      let fk, sh = fake () in
      let f = fk.script in
      with_daemon sh (fun d ->
          ignore (text (attach d));
          Fake_helper.queue_scan f [ gi "i32" 1 ];
          ignore (text (call d "find" [ "61" ]));
          Fake_helper.queue_peek f "i32" [ { addr = 0x10008L; v = Some (Vint 61L) } ];
          ignore (text (call d "results" []));
          Fake_helper.queue_write f ~prev:(Vint 61L) ~prev_bits:0x3dL;
          Fake_helper.queue_error f "read" { code; message = "" };
          let r = call d "set" [ "#1"; "999" ] in
          let two = "set #1 to 999 (was 61)\n" ^ why in
          check_str (code ^ ": two lines") two (text r);
          check_json (code ^ ": held null") (`Assoc [ ("message", `String two); ("held", `Null) ]) (json r);
          check_err (code ^ ": the next command") not_attached (call d "undo" []);
          check_str (code ^ ": status") "not attached" (text (call d "status" []));
          let open Yojson.Safe.Util in
          let set = index 2 (json (call d "log" [] ~opts:[ ("n", "3") ])) in
          check_str (code ^ ": the set's event") "set" (member "cmd" set |> to_string);
          check_json (code ^ ": its write")
            (`Assoc [ ("addr", `String "0x10008"); ("type", `String "i32"); ("prev_bits", `String "0x0000003d"); ("new_bits", `String "0x000003e7") ])
            (member "write" set);
          check_bool (code ^ ": its held, null") true (List.mem_assoc "held" (to_assoc set) && member "held" set = `Null)))
    [ ("target_gone", "the game closed right after, so moth couldn't check it."); ("helper_lost", "moth lost contact with its helper right after, so it couldn't check it.") ]

(* a set's event says whether the write held; every event lists the options it was given *)
let events_say_more () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_scan f [ gi "i32" 1 ];
      ignore (text (call d "find" [ "6" ]));
      Fake_helper.queue_peek f "i32" [ { addr = 0x10008L; v = Some (Vint 6L) } ];
      ignore (text (call d "results" [] ~opts:[ ("n", "5"); ("from", "1") ]));
      let set prev read =
        Fake_helper.queue_write f ~prev:(Vint prev) ~prev_bits:prev;
        Option.iter (fun v -> Fake_helper.queue_read f (Vint v)) read;
        ignore (text (call d "set" [ "1"; "9" ]))
      in
      (* put straight back; already reads 8; held; not read back (the fake's read_failed) *)
      set 6L (Some 6L);
      set 6L (Some 8L);
      set 8L (Some 9L);
      set 9L None;
      check_err "usage, with an option" "results -n needs a number from 1 to 200" (call d "results" [] ~opts:[ ("n", "0") ]);
      let open Yojson.Safe.Util in
      let ev = json (call d "log" [] ~opts:[ ("n", "7") ]) in
      (* newest first: results -n 0, the four sets, results -n 5 --from 1, find *)
      let held i = member "held" (index i ev) in
      check_json "not read back: null" `Null (held 1);
      check_bool "null, not left out" true (List.mem "held" (keys (index 1 ev)));
      check_json "held" (`Bool true) (held 2);
      check_json "already reads 8" (`Bool false) (held 3);
      check_json "put straight back" (`Bool false) (held 4);
      check_strs "a set's fields" [ "seq"; "ts"; "actor"; "cmd"; "args"; "ok"; "summary"; "held"; "write" ] (keys (index 4 ev));
      check_json "the options" (`Assoc [ ("n", `String "5"); ("from", `String "1") ]) (member "opts" (index 5 ev));
      check_strs "where they go" [ "seq"; "ts"; "actor"; "cmd"; "args"; "opts"; "ok"; "summary" ] (keys (index 5 ev));
      check_json "an error's options too" (`Assoc [ ("n", `String "0") ]) (member "opts" (index 0 ev));
      check_bool "none given, none listed" false (List.mem "opts" (keys (index 6 ev)));
      check_bool "nor held, for another command" false (List.mem "held" (keys (index 5 ev))))

let closed_game () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_read f (Vint 15L);
      ignore (text (call d "keep" [ "0x6c551068"; "15"; "xp" ]));
      Fake_helper.queue_scan f [ gi "i32" 3 ];
      ignore (text (call d "find" [ "100" ]));
      fk.statuses <- 0;
      check_str "still there" (attached ^ "\n3 matches (3 whole number)") (text (call d "status" []));
      check_int "the helper asked once" 1 fk.statuses;
      fk.alive <- false;
      let r = call d "status" [] in
      check_str "closed" "not attached. disco.exe closed." (text r);
      check_json "closed json" (`Assoc [ ("attached", `Null); ("hunt", `Null); ("closed", `String "disco.exe") ]) (json r);
      check_int "the helper let go of the game" 1 fk.detaches;
      check_err "kept" not_attached (call d "kept" []);
      check_err "results" not_attached (call d "results" []);
      check_str "said once" "not attached" (text (call d "status" []));
      check_int "not asked when nothing is attached" 2 fk.statuses;
      check_int "the helper kept" 0 fk.closed;
      (* attached again, nothing of the old game's is left *)
      ignore (text (attach d));
      check_str "no hunt" attached (text (call d "status" []));
      check_str "nothing kept" nothing_kept (text (call d "kept" []));
      (* a detach that fails changes nothing *)
      fk.alive <- false;
      fk.detach_fails <- true;
      check_str "closed, the detach failing" "not attached. disco.exe closed." (text (call d "status" []));
      check_int "asked to let go" 2 fk.detaches;
      check_int "the helper still kept" 0 fk.closed)

let status_helper_lost () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      fk.status_lost <- true;
      check_err "lost" "lost contact with moth's helper. attach again." (call d "status" []);
      check_int "helper closed" 1 fk.closed;
      check_str "status" "not attached" (text (call d "status" []));
      fk.status_lost <- false;
      ignore (text (attach d));
      check_strs "a new helper" [ "uhmm"; "uhmm" ] fk.started)

(* MOTH_BUDGET_MB is read when the daemon starts *)
let budget () =
  List.iter
    (fun (v, want) ->
      let fk, sh = fake () in
      with_env "MOTH_BUDGET_MB" v (fun () ->
          with_daemon sh (fun d ->
              hunt_for_three d fk.script 3;
              Alcotest.(check (option int)) ("MOTH_BUDGET_MB=" ^ v) want (Fake_helper.last_scan_budget fk.script))))
    [ ("64", Some 64); ("abc", None); ("1", Some 1); ("65536", Some 65536); ("0", None); ("65537", None); ("", None) ]

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

let parse_results_args () =
  let p = Client.parse_args in
  let same n w g = check_bool n true (w = g) in
  same "from and near"
    (Ok { Client.cmd = "results"; args = []; opts = [ ("n", "5"); ("from", "21"); ("near", "xp") ]; json = true })
    (p [ "results"; "-n"; "5"; "--from"; "21"; "--near"; "xp"; "--json" ]);
  same "near an address" (Ok { Client.cmd = "results"; args = []; opts = [ ("near", "0x6c551068") ]; json = false }) (p [ "results"; "--near"; "0x6c551068" ]);
  same "from needs a value" (Error "--from needs a value") (p [ "results"; "--from" ]);
  same "near needs a value" (Error "--near needs a value") (p [ "results"; "--near" ]);
  same "-n needs a value" (Error "-n needs a value") (p [ "results"; "-n" ])

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
          t "write record" write_record;
          t "attach again" reattach;
          t "crossover missing" crossover_missing;
          t "usage" usage;
          t "unexpected error" unexpected;
          t "results in pages" pages;
          t "results near a value, in pages" near_pages;
          t "results usage" results_usage;
          t "keep, kept, set by name, forget" keep_by_name;
          t "keep by address" keep_by_address;
          t "keep refused" keep_refused;
          t "keep a bare result number" keep_a_bare_number;
          t "a set the game put back" set_put_back;
          t "a write that could not be checked" a_write_that_could_not_be_checked;
          t "events say held, and the options" events_say_more;
          t "status notices a closed game" closed_game;
          t "status, helper lost" status_helper_lost;
          t "memory budget" budget;
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
      ("client", [ t "parse args" parse_args; t "parse args: results options" parse_results_args; t "home" home ]);
    ]
