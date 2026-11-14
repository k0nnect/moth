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

(* a launch that records its calls (newest first) and starts nothing *)
let fake_launch ?(result = Ok ()) () =
  let calls = ref [] in
  ((fun ~bottle ~exe ~args ->
      calls := (bottle, exe, args) :: !calls;
      result),
   calls)

let no_launch ~bottle:_ ~exe:_ ~args:_ = Ok ()

let start ?(home = fresh_home ()) ?(launch = no_launch) start_helper =
  let thread = Thread.create (fun () -> Daemon.run ~home ~start_helper ~launch) () in
  wait_up (sock home);
  { home; thread }

let call ?(actor = "user") ?(opts = []) d cmd args = Client.call ~autostart:false ~home:d.home ~actor ~cmd ~args ~opts ()
let text = function Ok (_, t) -> t | Error e -> Alcotest.failf "unexpected error: %s" e
let json = function Ok (j, _) -> j | Error e -> Alcotest.failf "unexpected error: %s" e
let check_err n msg = function Error e -> check_str n msg e | Ok (_, t) -> Alcotest.failf "%s: expected the error %S, got %S" n msg t
let check_json n w g = check_str n (Yojson.Safe.to_string w) (Yojson.Safe.to_string g)

(* run first when a daemon is stopped: the waiter tests put their processes away, so none is
   writing in the home that is removed *)
let before_daemon_stops = ref (fun () -> ())

let with_daemon ?home ?launch start_helper f =
  let d = start ?home ?launch start_helper in
  Fun.protect
    ~finally:(fun () ->
      !before_daemon_stops ();
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
      check_json "status json" (`Assoc [ ("attached", `Null); ("hunt", `Null); ("proto", `Int 2); ("quiet", `Bool false) ]) (json (call d "status" []));
      check_str "detach" "not attached" (text (call d "detach" [])))

let attaching () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      check_str "attach" attached (text (attach d ~what:"DISCO.EXE"));
      check_json "attach json" (`Assoc [ ("message", `String attached) ]) (json (attach d));
      check_strs "one helper, in the bottle" [ "uhmm" ] fk.started;
      check_str "status" attached (text (call d "status" []));
      check_json "status json"
        (`Assoc [ ("attached", `Assoc [ ("pid", `Int 208); ("name", `String "disco.exe"); ("bits", `Int 64); ("bottle", `String "uhmm") ]); ("hunt", `Null); ("proto", `Int 2); ("quiet", `Bool false) ])
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
        (`Assoc
          [
            ("main", `Int 3);
            ("wide", `Int 7);
            ("by_tag", `List [ `List [ `String "whole number"; `Int 3 ] ]);
            ("can_compare", `Bool true);
            ("groups", `List [ group_json "i32" 3; group_json ~tier:"wide" "u8" 7 ]);
            ("blind", `Bool false);
          ])
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
      check_json "set without #" (`Assoc [ ("message", `String "set #2 (0x2000) to 5 (was 999)"); ("held", `Bool true) ]) (json (call d "set" [ "2"; "5" ]));
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
      check_strs "fields" [ "seq"; "ts"; "actor"; "act"; "cmd"; "args"; "ok"; "summary" ] (keys (index 0 ev));
      check_int "seq" 4 (index 0 ev |> member "seq" |> to_int);
      check_bool "ts" true (index 0 ev |> member "ts" |> to_number > 1e9);
      check_bool "error logged" false (index 1 ev |> member "ok" |> to_bool);
      check_str "error summary" "find needs a number" (index 1 ev |> member "summary" |> to_string);
      check_bool "a failed acting command acted" true (index 1 ev |> member "act" |> to_bool);
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
      check_strs "fields" [ "seq"; "ts"; "actor"; "act"; "cmd"; "args"; "ok"; "summary"; "write" ] (keys (index 1 ev));
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
      (match Daemon.run ~home:d.home ~start_helper:sh ~launch:no_launch with
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
  (match Daemon.run ~home ~start_helper:sh ~launch:no_launch with () -> Alcotest.fail "the daemon ran" | exception Failure m -> check_str "daemon" msg m);
  check_bool "nothing created" false (Sys.file_exists home);
  let home = "/dev/null/moth" in
  let msg = "couldn't create /dev/null/moth (Not a directory). set MOTH_HOME to a directory you can write to." in
  check_err "home can't be made" msg (Client.call ~home ~actor:"user" ~cmd:"status" ~args:[] ~opts:[] ());
  match Daemon.run ~home ~start_helper:sh ~launch:no_launch with () -> Alcotest.fail "the daemon ran" | exception Failure m -> check_str "daemon, home can't be made" msg m

let autostart () =
  let _, sh = fake () in
  let home = fresh_home () in
  Unix.mkdir home 0o700;
  (* a stale socket: a plain file at the socket's path, no daemon *)
  close_out (open_out (sock home));
  let thread = ref None in
  let spawn_daemon ~home = thread := Some (Thread.create (fun () -> Daemon.run ~home ~start_helper:sh ~launch:no_launch) ()) in
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
      check_str "keep" "kept #1 (0x10008) as xp (60, whole number)" (text r);
      check_json "keep json" (`Assoc [ ("message", `String "kept #1 (0x10008) as xp (60, whole number)") ]) (json r);
      Fake_helper.queue_read f (Vint 60L);
      Fake_helper.queue_read f (Vint 60L);
      check_str "kept" "xp  60  0x10008  whole number" (text (call d "kept" []));
      check_json "kept json"
        (`List
           [
             `Assoc
               [ ("name", `String "xp"); ("addr", `String "0x10008"); ("tag", `String "whole number"); ("shown", `String "60"); ("set_to", `Null); ("put_back", `Bool false); ("note", `String "") ];
           ])
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
            `Assoc [ ("name", `String "xp"); ("addr", `String "0x6c551068"); ("tag", `String "whole number"); ("shown", `String "15"); ("set_to", `Null); ("put_back", `Bool false); ("note", `String "") ];
            `Assoc [ ("name", `String "v1"); ("addr", `String "0x6c551068"); ("tag", `String "whole number"); ("shown", `String "?"); ("set_to", `Null); ("put_back", `Bool false); ("note", `String "") ];
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
      check_str "keep 1 xp" "kept #1 (0x1000) as xp (3, whole number)" (text (call d "keep" [ "1"; "xp" ]));
      check_str "keep 1" "kept #1 (0x1000) as v1 (3, whole number)" (text (call d "keep" [ "1" ]));
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
      let two = "set #1 (0x10008) to 9 (was 6)\nthe game put it straight back to 6. it's worked out from something else, so writing here won't stick." in
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
          let two = "set #1 (0x10008) to 999 (was 61)\n" ^ why in
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
      check_strs "a set's fields" [ "seq"; "ts"; "actor"; "act"; "cmd"; "args"; "ok"; "summary"; "held"; "write" ] (keys (index 4 ev));
      check_json "the options" (`Assoc [ ("n", `String "5"); ("from", `String "1") ]) (member "opts" (index 5 ev));
      check_strs "where they go" [ "seq"; "ts"; "actor"; "act"; "cmd"; "args"; "opts"; "ok"; "summary" ] (keys (index 5 ev));
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
      check_json "closed json" (`Assoc [ ("attached", `Null); ("hunt", `Null); ("closed", `String "disco.exe"); ("proto", `Int 2); ("quiet", `Bool false) ]) (json r);
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

(* ---- stage 1c: snapshot ---- *)

let blind = [ gi "i32" 1000; gi "f32" 1000; gi "i16" 2000; gi "i64" 500; gi "f64" 500; gi "u8" 4000 ]
let snapshot_text = "snapshot of 4 kb taken. change the value in the game, then tell moth how it moved: moth next down, up, same or changed."
let no_step_yet = "snapshot taken, no step yet. change the value in the game, then tell moth how it moved: moth next down, up, same or changed."

let a_snapshot_hunt () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      check_err "not attached" not_attached (call d "snapshot" []);
      ignore (text (attach d));
      Fake_helper.queue_snapshot f ~stats:{ Fake_helper.zero_stats with bytes = 4096 } blind;
      let r = call d "snapshot" [] in
      check_str "text" snapshot_text (text r);
      let open Yojson.Safe.Util in
      check_json "notes" (`List [ `String "snapshot" ]) (member "notes" (json r));
      check_int "bytes" 4096 (member "bytes" (json r) |> to_int);
      check_bool "the blind plan" true
        (Fake_helper.last_snapshot_groups f = [ ("i32", "i32", 4); ("f32", "f32", 4); ("i16", "i16", 2); ("i64", "i64", 8); ("f64", "f64", 8); ("u8", "u8", 1) ]);
      check_int "one call" 1 (Fake_helper.snapshot_calls f);
      (* R17: before a step, status says so in the counts line's place, and its json says blind *)
      check_str "status" (attached ^ "\n" ^ no_step_yet) (text (call d "status" []));
      let hunt = member "hunt" (json (call d "status" [])) in
      check_int "status json" 5000 (member "main" hunt |> to_int);
      check_bool "status json: blind" true (member "blind" hunt |> to_bool);
      check_err "nothing to list yet" "nothing to list yet. tell moth how the value moved first: moth next down, up, same or changed." (call d "results" []);
      Fake_helper.queue_narrow f ~applied:true [ gi "i32" 3; gi "f32" 2; gi "i16" 0; gi "i64" 0; gi "f64" 0; gi "u8" 0 ];
      check_str "next down" "5 matches (3 whole number, 2 decimal)" (text (call d "next" [ "down" ]));
      let r = call d "status" [] in
      check_str "status after a step" (attached ^ "\n5 matches (3 whole number, 2 decimal)") (text r);
      check_bool "status json after a step: not blind" false (member "hunt" (json r) |> member "blind" |> to_bool);
      Fake_helper.queue_peek f "i32" [ { addr = 0x10008L; v = Some (Vint 5L) } ];
      Fake_helper.queue_peek f "f32" [ { addr = 0x10010L; v = Some (Vfloat 5.) } ];
      check_bool "results lists" true (String.length (text (call d "results" [])) > 0);
      let log = json (call d "log" [] ~opts:[ ("n", "9") ]) in
      check_strs "one event per command"
        [ "snapshot"; "attach"; "snapshot"; "status"; "status"; "results"; "next"; "status"; "results" ]
        (List.rev_map (fun e -> member "cmd" e |> to_string) (to_list log)))

(* R17: a step that keeps nothing is no step, so status still says none was taken; a find that
   replaces the snapshot hunt is not blind *)
let status_of_a_snapshot_hunt () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      let status () =
        let r = call d "status" [] in
        (text r, Yojson.Safe.Util.(member "hunt" (json r) |> member "blind" |> to_bool))
      in
      Fake_helper.queue_snapshot f blind;
      ignore (text (call d "snapshot" []));
      Fake_helper.queue_narrow f ~applied:false [];
      ignore (text (call d "next" [ "up" ]));
      let said, blind_now = status () in
      check_str "after a dead end" (attached ^ "\n" ^ no_step_yet) said;
      check_bool "after a dead end: blind" true blind_now;
      Fake_helper.queue_scan f [ gi "i32" 3 ];
      ignore (text (call d "find" [ "100" ]));
      let said, blind_now = status () in
      check_str "a find replaces it" (attached ^ "\n3 matches (3 whole number)") said;
      check_bool "a find replaces it: not blind" false blind_now)

let snapshot_usage () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      check_err "extra" "snapshot takes no arguments" (call d "snapshot" [ "extra" ]);
      check_int "the helper was not called" 0 (Fake_helper.snapshot_calls fk.script);
      check_bool "usage check" true (Daemon.check_usage ~cmd:"snapshot" ~args:[] ~opts:[] = Ok ()))

let snapshot_too_big () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      hunt_for_three d f 3;
      Fake_helper.queue_error f "snapshot" { code = "too_big"; message = "4516605952 to snapshot, limit 1073741824" };
      let msg =
        "the game has 4.2 gb of memory to snapshot, more than moth's limit of 1.0 gb. the limit is set when moth starts: moth stop, then run moth again with MOTH_SNAPSHOT_MB=<mb> (stopping forgets the hunt and the kept values). or hunt with a number: moth find <number>"
      in
      check_err "too big" msg (call d "snapshot" []);
      check_str "the hunt there was" (attached ^ "\n3 matches (3 whole number)") (text (call d "status" []));
      let log = json (call d "log" [] ~opts:[ ("n", "3") ]) in
      let open Yojson.Safe.Util in
      check_bool "logged as an error" false (index 1 log |> member "ok" |> to_bool);
      check_str "with the text" msg (index 1 log |> member "summary" |> to_string))

let kept_put_back () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_scan f [ gi "i32" 1 ];
      ignore (text (call d "find" [ "10" ]));
      Fake_helper.queue_peek f "i32" [ { addr = 0x10008L; v = Some (Vint 10L) } ];
      ignore (text (call d "results" []));
      ignore (text (call d "keep" [ "#1"; "hp" ]));
      Fake_helper.queue_read f (Vint 10L);
      check_json "never written by name"
        (`List
           [
             `Assoc
               [ ("name", `String "hp"); ("addr", `String "0x10008"); ("tag", `String "whole number"); ("shown", `String "10"); ("set_to", `Null); ("put_back", `Bool false); ("note", `String "") ];
           ])
        (json (call d "kept" []));
      Fake_helper.queue_write f ~prev:(Vint 10L) ~prev_bits:10L;
      Fake_helper.queue_read f (Vint 20L);
      ignore (text (call d "set" [ "hp"; "20" ]));
      (* the game wrote 10 back *)
      Fake_helper.queue_read f (Vint 10L);
      Fake_helper.queue_read f (Vint 10L);
      check_json "put back"
        (`List
           [
             `Assoc
               [ ("name", `String "hp"); ("addr", `String "0x10008"); ("tag", `String "whole number"); ("shown", `String "10"); ("set_to", `String "20"); ("put_back", `Bool true); ("note", `String "you set it to 20; it is back to what it was before.") ];
           ])
        (json (call d "kept" []));
      check_bool "the text line" true (String.ends_with ~suffix:"you set it to 20; it is back to what it was before." (text (call d "kept" []))))

(* the panels read what a hunt holds from status, and a value's note from kept *)
let status_for_panels () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_scan f [ gi "i32" 100 ];
      let found = json (call d "find" [ "100" ]) in
      let hunt = Yojson.Safe.Util.(member "hunt" (json (call d "status" []))) in
      check_json "by_tag as the find gave it" Yojson.Safe.Util.(member "by_tag" found) Yojson.Safe.Util.(member "by_tag" hunt);
      check_json "can compare" (`Bool true) Yojson.Safe.Util.(member "can_compare" hunt);
      Fake_helper.queue_scan f [ { (gi "i32" 100) with has_values = false } ];
      ignore (text (call d "find" [ "100" ]));
      let hunt = Yojson.Safe.Util.(member "hunt" (json (call d "status" []))) in
      check_json "cannot compare" (`Bool false) Yojson.Safe.Util.(member "can_compare" hunt))

let kept_says_its_note () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      hunt_for_three d f 1;
      Fake_helper.queue_peek f "i32" (threes 1);
      ignore (text (call d "results" []));
      ignore (text (call d "keep" [ "1"; "hp" ]));
      Fake_helper.queue_read f (Vint 3L);
      check_json "never set" (`String "") Yojson.Safe.Util.(index 0 (json (call d "kept" [])) |> member "note");
      Fake_helper.queue_write f ~prev:(Vint 3L) ~prev_bits:3L;
      Fake_helper.queue_read f (Vint 20L);
      ignore (text (call d "set" [ "hp"; "20" ]));
      Fake_helper.queue_read f (Vint 3L);
      Fake_helper.queue_read f (Vint 3L);
      check_json "set and back" (`String "you set it to 20; it is back to what it was before.") Yojson.Safe.Util.(index 0 (json (call d "kept" [])) |> member "note"))

(* --at pins a click to the address its row showed *)
let at_guards_a_row () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      hunt_for_three d f 3;
      (* --at finds the row afresh: row 3 alone, which is at 0x1008 *)
      let row3 () = Fake_helper.queue_peek f "i32" [ List.nth (threes 3) 2 ] in
      let at a = [ ("at", a) ] in
      let moved n a = Printf.sprintf "row %d is not at %s any more: the list changed. look again: moth results" n a in
      row3 ();
      Fake_helper.queue_write f ~prev:(Vint 3L);
      Fake_helper.queue_read f (Vint 9L);
      check_str "set at its address" "set #3 (0x1008) to 9 (was 3)" (text (call d "set" [ "3"; "9" ] ~opts:(at "0x1008")));
      check_bool "row 3 alone" true (Fake_helper.last_peek f "i32" = (2, 1));
      check_bool "wrote" true (Fake_helper.last_write f = (0x1008L, "i32", Put_value (Vint 9L)));
      row3 ();
      check_err "set elsewhere" (moved 3 "0x1000") (call d "set" [ "3"; "7" ] ~opts:(at "0x1000"));
      check_bool "no write" true (Fake_helper.last_write f = (0x1008L, "i32", Put_value (Vint 9L)));
      row3 ();
      check_err "keep elsewhere" (moved 3 "0x1000") (call d "keep" [ "3"; "hp" ] ~opts:(at "0x1000"));
      check_str "nothing kept" nothing_kept (text (call d "kept" []));
      row3 ();
      Fake_helper.queue_read f (Vint 9L);
      check_str "keep at its address" "kept #3 (0x1008) as hp (9, whole number)" (text (call d "keep" [ "3"; "hp" ] ~opts:(at "0x1008")));
      let row_only = "--at goes with a row number" in
      check_err "set by name" row_only (call d "set" [ "hp"; "9" ] ~opts:(at "0x10"));
      check_err "keep by address" row_only (call d "keep" [ "0x1008"; "9" ] ~opts:(at "0x1008"));
      let not_address = "--at needs an address like 0x7ffe1234" in
      check_err "set, not an address" not_address (call d "set" [ "3"; "9" ] ~opts:(at "12"));
      check_err "keep, not an address" not_address (call d "keep" [ "3" ] ~opts:(at "0x"));
      check_err "another command" "--at goes with keep and set" (call d "results" [] ~opts:(at "0x10")))

(* MOTH_SNAPSHOT_MB is read when the daemon starts *)
let snapshot_cap () =
  List.iter
    (fun (v, want) ->
      let fk, sh = fake () in
      with_env "MOTH_SNAPSHOT_MB" v (fun () ->
          with_daemon sh (fun d ->
              ignore (text (attach d));
              Fake_helper.queue_snapshot fk.script blind;
              ignore (text (call d "snapshot" []));
              Alcotest.(check (option int)) ("MOTH_SNAPSHOT_MB=" ^ v) want (Fake_helper.last_snapshot_cap fk.script))))
    [ ("2048", Some 2048); ("abc", None); ("0", None); ("", None); ("1", Some 1); ("1048576", Some 1048576); ("1048577", None); ("-5", None); ("12abc", None); (" 2048", None) ]

(* ---- stage 2a: the feed, and reads that leave no trace ---- *)

let send_line s j =
  let line = Yojson.Safe.to_string j ^ "\n" in
  ignore (Unix.write_substring s line 0 (String.length line))

(* a reply as [call] gives it *)
let answer_of r =
  let open Yojson.Safe.Util in
  if member "ok" r = `Bool true then Ok (member "result" r, to_string (member "text" r)) else Error (to_string (member "error" r))

(* the protocol field every client of this build sends *)
let this_build = [ ("proto", `Int Daemon.protocol) ]

(* one request with these fields (and [proto], this build's unless given) on its own connection;
   its reply *)
let request ?(proto = this_build) d fields =
  let s = Unix.socket Unix.PF_UNIX Unix.SOCK_STREAM 0 in
  Unix.connect s (Unix.ADDR_UNIX (sock d.home));
  let ic = Unix.in_channel_of_descr s in
  Fun.protect
    ~finally:(fun () -> close_in_noerr ic)
    (fun () ->
      send_line s (`Assoc ((("id", `Int 1) :: proto) @ fields));
      answer_of (Yojson.Safe.from_string (input_line ic)))

let strings l = `List (List.map (fun a -> `String a) l)

(* a request sent with record: false *)
let unrecorded ?(opts = []) d cmd args =
  request d [ ("cmd", `String cmd); ("args", strings args); ("opts", `Assoc (List.map (fun (k, v) -> (k, `String v)) opts)); ("record", `Bool false) ]

(* a connection that sent watch, read a line at a time; a line must come within 5 seconds *)
type watcher = { wfd : Unix.file_descr; wic : in_channel }

let watcher ?after ?(proto = this_build) d =
  let s = Unix.socket Unix.PF_UNIX Unix.SOCK_STREAM 0 in
  Unix.connect s (Unix.ADDR_UNIX (sock d.home));
  Unix.setsockopt_float s Unix.SO_RCVTIMEO 5.;
  let opts = match after with Some k -> [ ("after", `Int k) ] | None -> [] in
  send_line s (`Assoc ((("id", `Int 1) :: proto) @ [ ("cmd", `String "watch"); ("opts", `Assoc opts) ]));
  { wfd = s; wic = Unix.in_channel_of_descr s }

let next_line w =
  match input_line w.wic with
  | l -> Yojson.Safe.from_string l
  | exception End_of_file -> Alcotest.fail "the daemon let go of the watcher"
  | exception Sys_error _ -> Alcotest.fail "the watcher heard nothing for 5 seconds"

(* reads what is left until the daemon closes the connection; false when it is still open after 5
   seconds *)
let let_go w =
  let buf = Bytes.create 65536 in
  let rec go () = match input w.wic buf 0 (Bytes.length buf) with 0 -> true | _ -> go () | exception Sys_error _ -> false in
  go ()

let unwatch w = close_in_noerr w.wic
let watching seq = `Assoc [ ("id", `Int 1); ("ok", `Bool true); ("result", `Assoc [ ("seq", `Int seq) ]); ("text", `String "") ]
let busy actor cmd args = `Assoc [ ("busy", `Assoc [ ("actor", `String actor); ("cmd", `String cmd); ("args", strings args) ]) ]
let event ev = `Assoc [ ("event", ev) ]
let seq_of line = Yojson.Safe.Util.(member "event" line |> member "seq" |> to_int)

(* events.jsonl, oldest first *)
let stored d =
  match open_in (Filename.concat d.home "events.jsonl") with
  | exception Sys_error _ -> []
  | ic ->
      let rec go acc = match input_line ic with l -> go (Yojson.Safe.from_string l :: acc) | exception End_of_file -> List.rev acc in
      Fun.protect ~finally:(fun () -> close_in ic) (fun () -> go [])

let a_watcher_sees_a_find () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      let w = watcher d in
      check_json "the reply: the newest seq" (watching 1) (next_line w);
      Fake_helper.queue_scan fk.script [ gi "i32" 3 ];
      ignore (text (call d "find" [ "3" ]));
      check_json "it starts" (busy "user" "find" [ "3" ]) (next_line w);
      let evs = stored d in
      check_int "events.jsonl gained a line" 2 (List.length evs);
      check_json "it ends: the line events.jsonl gained" (event (List.nth evs 1)) (next_line w);
      unwatch w)

let watching_after () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      ignore (text (call d "status" []));
      ignore (text (call d "status" []));
      let w = watcher ~after:1 d in
      check_json "the reply" (watching 3) (next_line w);
      let evs = stored d in
      check_json "the older of the two" (event (List.nth evs 1)) (next_line w);
      check_json "then the newer" (event (List.nth evs 2)) (next_line w);
      Fake_helper.queue_scan fk.script [ gi "i32" 3 ];
      ignore (text (call d "find" [ "3" ]));
      check_json "then live lines" (busy "user" "find" [ "3" ]) (next_line w);
      check_json "and the event" (event (List.nth (stored d) 3)) (next_line w);
      unwatch w;
      let w = watcher ~after:4 d in
      check_json "after the newest" (watching 4) (next_line w);
      ignore (text (call d "status" []));
      check_json "nothing stored comes" (busy "user" "status" []) (next_line w);
      unwatch w;
      (* Client.watch passes each feed line on until the function says stop *)
      let seen = ref [] in
      Alcotest.(check (result unit string))
        "Client.watch" (Ok ())
        (Client.watch ~home:d.home ~after:3 (fun l ->
             seen := l :: !seen;
             List.length !seen < 2));
      Alcotest.(check (list int)) "two events, oldest first" [ 4; 5 ] (List.rev_map seq_of !seen))

(* the backlog is longer than a socket holds, so it goes out as the watcher reads *)
let watching_after_many () =
  let home = fresh_home () in
  Unix.mkdir home 0o700;
  let oc = open_out (Filename.concat home "events.jsonl") in
  for seq = 1 to 1005 do
    let ev = `Assoc [ ("seq", `Int seq); ("ts", `Float 1e9); ("actor", `String "user"); ("cmd", `String "status"); ("args", `List []); ("ok", `Bool true); ("summary", `String "not attached") ] in
    output_string oc (Yojson.Safe.to_string ev ^ "\n")
  done;
  close_out oc;
  let _, sh = fake () in
  with_daemon ~home sh (fun d ->
      let w = watcher ~after:0 d in
      check_json "the reply" (watching 1005) (next_line w);
      let rec read n acc = if n = 0 then List.rev acc else read (n - 1) (seq_of (next_line w) :: acc) in
      Alcotest.(check (list int)) "the newest 1,000, oldest first" (List.init 1000 (fun i -> i + 6)) (read 1000 []);
      ignore (text (call d "status" []));
      check_json "then live lines" (busy "user" "status" []) (next_line w);
      check_int "with the next seq" 1006 (seq_of (next_line w));
      unwatch w)

let reads_that_leave_no_trace () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      hunt_for_three d f 2;
      let w = watcher d in
      check_json "the reply" (watching 2) (next_line w);
      check_str "status" (attached ^ "\n2 matches (2 whole number)") (text (unrecorded d "status" []));
      Fake_helper.queue_peek f "i32" (threes 2);
      check_int "results" 2 (List.length (lines (text (unrecorded d "results" [] ~opts:[ ("all", "true"); ("n", "200") ]))));
      check_str "ps" "208  disco.exe\n300  explorer.exe" (text (unrecorded d "ps" [] ~opts:[ ("bottle", "uhmm") ]));
      check_str "kept" nothing_kept (text (unrecorded d "kept" []));
      check_int "log" 2 (List.length (lines (text (unrecorded d "log" []))));
      check_err "an error, unrecorded too" "results -n needs a number from 1 to 200" (unrecorded d "results" [] ~opts:[ ("n", "0") ]);
      check_int "events.jsonl as it was, watch included" 2 (List.length (stored d));
      ignore (text (call d "status" []));
      check_json "the watcher heard nothing until now" (busy "user" "status" []) (next_line w);
      check_int "the seq was where it was" 3 (seq_of (next_line w));
      check_int "one line more" 3 (List.length (stored d));
      unwatch w)

let only_reads_go_unrecorded () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      let refused = "only commands that change nothing can go unrecorded" in
      check_err "find" refused (unrecorded d "find" [ "3" ]);
      check_int "the helper was not called" 0 (Fake_helper.scan_calls fk.script);
      List.iter
        (fun (cmd, args) -> check_err cmd refused (unrecorded d cmd args))
        [ ("attach", [ "disco.exe" ]); ("snapshot", []); ("next", [ "down" ]); ("keep", [ "1" ]); ("forget", [ "xp" ]); ("set", [ "1"; "5" ]); ("undo", []); ("detach", []); ("stop", []) ];
      check_bool "nor for any of them" true (Fake_helper.calls fk.script = []);
      check_int "attached once" 1 fk.attaches;
      check_int "nothing recorded" 1 (List.length (stored d));
      check_str "still attached, still serving" attached (text (call d "status" [])))

(* ---- who spoke, and whether it acted ---- *)

let act_of ev = Yojson.Safe.Util.(member "act" ev |> to_bool)
let cmd_of ev = Yojson.Safe.Util.(member "cmd" ev |> to_string)

let an_agent_is_named () =
  let _, sh = fake () in
  with_daemon sh (fun d ->
      let w = watcher d in
      check_json "the reply" (watching 0) (next_line w);
      ignore (text (call ~actor:"agent" d "attach" [ "disco.exe" ] ~opts:[ ("bottle", "uhmm") ]));
      check_json "its busy line carries it" (busy "agent" "attach" [ "disco.exe" ]) (next_line w);
      let ev = List.hd (stored d) in
      check_str "its event carries it" "agent" Yojson.Safe.Util.(member "actor" ev |> to_string);
      unwatch w)

let events_say_whether_they_acted () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      let acts = ref [] in
      let run cmd args =
        ignore (call d cmd args);
        acts := !acts @ [ (cmd, act_of (List.nth (stored d) (List.length (stored d) - 1))) ]
      in
      run "games" [];
      run "bottles" [];
      run "status" [];
      ignore (text (attach d));
      Fake_helper.queue_scan f [ gi "i32" 3 ];
      run "find" [ "3" ];
      Fake_helper.queue_peek f "i32" (threes 3);
      run "results" [];
      Fake_helper.queue_read f (Vint 3L);
      run "keep" [ "1"; "hp" ];
      Fake_helper.queue_read f (Vint 3L);
      run "kept" [];
      Fake_helper.queue_write f ~prev:(Vint 3L);
      Fake_helper.queue_read f (Vint 9L);
      run "set" [ "hp"; "9" ];
      Fake_helper.queue_write f ~prev:(Vint 9L);
      run "undo" [];
      run "log" [];
      run "forget" [ "hp" ];
      run "status" [];
      run "detach" [];
      let want =
        [ ("games", false); ("bottles", false); ("status", false); ("find", true); ("results", false); ("keep", true); ("kept", false);
          ("set", true); ("undo", true); ("log", false); ("forget", true); ("status", false); ("detach", true) ]
      in
      Alcotest.(check (list (pair string bool))) "which acted" want !acts;
      let attach_ev = List.find (fun ev -> cmd_of ev = "attach") (stored d) in
      check_bool "attach acted" true (act_of attach_ev))

let a_closed_game_acts () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      fk.alive <- false;
      ignore (unrecorded d "status" []);
      let ev = List.nth (stored d) 1 in
      check_str "a status" "status" (cmd_of ev);
      check_bool "that let go of the game, so it acted" true (act_of ev))

let status_says_its_protocol () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      let proto r = Yojson.Safe.Util.(json r |> member "proto") in
      check_json "not attached" (`Int 2) (proto (call d "status" []));
      ignore (text (attach d));
      check_json "no hunt" (`Int 2) (proto (call d "status" []));
      Fake_helper.queue_scan f [ gi "i32" 3 ];
      ignore (text (call d "find" [ "3" ]));
      check_json "a hunt" (`Int 2) (proto (call d "status" []));
      check_int "Daemon.protocol" 2 Daemon.protocol;
      fk.alive <- false;
      check_json "just closed" (`Int 2) (proto (call d "status" [])))

(* a request with no proto, or another number, is refused and nothing of it runs; stop and status
   are served whatever a client sends *)
let another_build_is_refused () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      let refused = "this moth is from another build than its background service. stop the service and it starts again by itself: moth stop" in
      let fields ?(record = true) cmd args opts =
        [ ("actor", `String "user"); ("cmd", `String cmd); ("args", strings args); ("opts", `Assoc (List.map (fun (k, v) -> (k, `String v)) opts)) ]
        @ if record then [] else [ ("record", `Bool false) ]
      in
      List.iter
        (fun (what, proto) ->
          check_err (what ^ ": attach") refused (request ~proto d (fields "attach" [ "disco.exe" ] [ ("bottle", "uhmm") ]));
          check_err (what ^ ": quiet on") refused (request ~proto d (fields "quiet" [ "on" ] []));
          check_err (what ^ ": an unrecorded read") refused (request ~proto d (fields ~record:false "games" [] []));
          check_err (what ^ ": a command that does not exist") refused (request ~proto d (fields "frobnicate" [] [])))
        [ ("no proto", []); ("proto 1", [ ("proto", `Int 1) ]); ("the next proto", [ ("proto", `Int (Daemon.protocol + 1)) ]); ("proto as text", [ ("proto", `String "2") ]) ];
      check_strs "no helper started" [] fk.started;
      check_int "nothing attached" 0 fk.attaches;
      check_bool "quiet not kept" false (Sys.file_exists (Filename.concat d.home "quiet"));
      check_int "nothing recorded" 0 (List.length (stored d));
      let w = watcher ~proto:[] d in
      check_json "a watch with no proto" (`Assoc [ ("id", `Int 1); ("ok", `Bool false); ("error", `String refused) ]) (next_line w);
      check_bool "is let go" true (let_go w);
      unwatch w;
      let w = watcher ~proto:[ ("proto", `Int 1) ] d in
      check_json "a watch with another proto" (`Assoc [ ("id", `Int 1); ("ok", `Bool false); ("error", `String refused) ]) (next_line w);
      unwatch w;
      check_int "still nothing recorded" 0 (List.length (stored d));
      check_str "status with no proto" "not attached" (text (request ~proto:[] d (fields "status" [] [])));
      check_json "says this build's proto" (`Int Daemon.protocol) Yojson.Safe.Util.(member "proto" (json (request ~proto:[ ("proto", `Int 1) ] d (fields ~record:false "status" [] []))));
      check_int "recorded as any status is" 1 (List.length (stored d));
      check_str "stop with no proto" "stopped" (text (request ~proto:[] d (fields "stop" [] [])));
      check_strs "recorded: the status and the stop" [ "status"; "stop" ] (List.map cmd_of (stored d)))

let client_watch_hooks () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_scan fk.script [ gi "i32" 3 ];
      ignore (text (call d "find" [ "3" ]));
      (* started comes with the reply's seq, before the stored events it is followed by *)
      let order = ref [] in
      let r =
        Client.watch ~home:d.home ~after:0
          ~started:(fun n -> order := !order @ [ Printf.sprintf "started %d" n ])
          (fun line ->
            order := !order @ [ Printf.sprintf "line %d" (seq_of line) ];
            List.length !order < 3)
      in
      check_bool "ended" true (r = Ok ());
      check_strs "started first" [ "started 2"; "line 1"; "line 2" ] !order;
      (* idle is called while nothing happens, and false ends the watch *)
      let t0 = Unix.gettimeofday () in
      let idles = ref 0 in
      let r = Client.watch ~home:d.home ~idle:(fun () -> incr idles; !idles < 2) (fun _ -> true) in
      check_bool "ended by idle" true (r = Ok ());
      check_int "called twice" 2 !idles;
      check_bool "within a second or so" true (Unix.gettimeofday () -. t0 < 2.))

let a_closed_game_is_recorded () =
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      let w = watcher d in
      check_json "the reply" (watching 1) (next_line w);
      fk.alive <- false;
      check_str "status, unrecorded" "not attached. disco.exe closed." (text (unrecorded d "status" []));
      let evs = stored d in
      check_int "recorded all the same" 2 (List.length evs);
      let ev = List.nth evs 1 in
      check_str "as a status" "status" Yojson.Safe.Util.(member "cmd" ev |> to_string);
      check_str "saying so" "not attached. disco.exe closed." Yojson.Safe.Util.(member "summary" ev |> to_string);
      check_json "and the watcher hears it" (event ev) (next_line w);
      check_str "status again, unrecorded" "not attached" (text (unrecorded d "status" []));
      check_int "not recorded" 2 (List.length (stored d));
      unwatch w)

(* An unrecorded read that makes the daemon let go of the game or its helper is recorded all the
   same, and its event, with no busy line before it, reaches watchers. [lose] makes the helper
   answer the read that way; [keeping] keeps a value first, for kept to read. *)
let a_read_that_loses_the_game ?(keeping = false) cmd lose answer () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      hunt_for_three d f 2;
      if keeping then begin
        Fake_helper.queue_peek f "i32" (threes 2);
        ignore (text (call d "results" []));
        ignore (text (call d "keep" [ "1"; "xp" ]))
      end;
      let before = List.length (stored d) in
      let w = watcher d in
      check_json "the reply" (watching before) (next_line w);
      lose fk;
      check_err "the answer" answer (unrecorded d cmd []);
      let evs = stored d in
      check_int "recorded all the same" (before + 1) (List.length evs);
      let ev = List.nth evs before in
      let open Yojson.Safe.Util in
      check_str "as the read" cmd (member "cmd" ev |> to_string);
      check_int "with the next seq" (before + 1) (member "seq" ev |> to_int);
      check_str "saying why" answer (member "summary" ev |> to_string);
      check_bool "and it acted" true (member "act" ev |> to_bool);
      check_json "the watcher hears it, with no busy line before it" (event ev) (next_line w);
      check_str "status, unrecorded" "not attached" (text (unrecorded d "status" []));
      check_int "which is not recorded" (before + 1) (List.length (stored d));
      unwatch w)

let game_closed = "the game closed. attach again once it's running."
let lost_contact = "lost contact with moth's helper. attach again."

(* the helper answers its next [op] (peek or read) with the error [code] *)
let answers_next op code (fk : fake) = Fake_helper.queue_error fk.script op { code; message = "" }

let sixteen_watchers () =
  let _, sh = fake () in
  with_daemon sh (fun d ->
      let ws =
        List.init 16 (fun i ->
            let w = watcher d in
            check_json (Printf.sprintf "watcher %d" (i + 1)) (watching 0) (next_line w);
            w)
      in
      let w = watcher d in
      check_json "the 17th" (`Assoc [ ("id", `Int 1); ("ok", `Bool false); ("error", `String "too many watchers") ]) (next_line w);
      check_bool "is let go" true (let_go w);
      unwatch w;
      Alcotest.(check (result unit string)) "Client.watch says so" (Error "too many watchers") (Client.watch ~home:d.home (fun _ -> true));
      unwatch (List.hd ws);
      let w = watcher d in
      check_json "one gone, a new one is taken" (watching 0) (next_line w);
      check_str "a status still answers" "not attached" (text (call d "status" []));
      List.iter unwatch (w :: List.tl ws))

let a_watcher_that_never_reads () =
  let _, sh = fake () in
  with_daemon sh (fun d ->
      let w = watcher d in
      check_json "the reply, the last line it reads" (watching 0) (next_line w);
      (* each status from this actor tells the watcher about 8 kb; 64 of them fill its socket many
         times over *)
      let actor = String.make 4000 'a' in
      let t0 = Unix.gettimeofday () in
      for _ = 1 to 64 do
        ignore (text (call ~actor d "status" []))
      done;
      check_bool "they did not wait for it" true (Unix.gettimeofday () -. t0 < 5.);
      let t0 = Unix.gettimeofday () in
      check_str "a status" "not attached" (text (call d "status" []));
      check_bool "answers within one second" true (Unix.gettimeofday () -. t0 < 1.);
      check_bool "the watcher was let go" true (let_go w);
      unwatch w)

let watch_with_no_daemon () =
  let home = fresh_home () in
  Alcotest.(check (result unit string)) "no daemon" (Error Client.not_running) (Client.watch ~home (fun _ -> true));
  check_bool "none started" false (Sys.file_exists home)

(* a watcher hears the stop, and Client.watch returns when the daemon goes *)
let watching_until_stop () =
  let _, sh = fake () in
  let d = start sh in
  ignore (text (call d "status" []));
  let seen = Atomic.make [] and ended = ref (Error "still watching") in
  let t = Thread.create (fun () -> ended := Client.watch ~home:d.home ~after:0 (fun l -> Atomic.set seen (l :: Atomic.get seen); true)) () in
  (* the stored status says it is watching *)
  let rec wait n =
    if Atomic.get seen = [] then begin
      if n = 0 then Alcotest.fail "Client.watch heard nothing";
      Thread.delay 0.01;
      wait (n - 1)
    end
  in
  wait 500;
  check_str "stop" "stopped" (text (call d "stop" []));
  Thread.join t;
  Thread.join d.thread;
  Alcotest.(check (result unit string)) "ends when the daemon goes" (Ok ()) !ended;
  let seen = List.rev (Atomic.get seen) in
  check_json "it heard the stop start" (busy "user" "stop" []) (List.nth seen 1);
  check_str "and end" "stop" Yojson.Safe.Util.(List.nth seen 2 |> member "event" |> member "cmd" |> to_string);
  check_int "and nothing else" 3 (List.length seen);
  rm d.home

(* ---- a row write names its own row ---- *)

let first_addr r = Yojson.Safe.Util.(index 0 (member "rows" (json r)) |> member "addr" |> to_string)
let moved_text n a = Printf.sprintf "row %d is not at %s any more: the list changed. look again: moth results" n a

(* the window's unrecorded poll comes between a command-line listing and its row write: the row
   numbers stay the ones the command line was shown (the whole-branch review's probe) *)
let a_poll_leaves_the_rows () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      hunt_for_three d f 3;
      (* the command line lists near 0x1008: #1 0x1008, #2 0x1004, #3 0x1000 *)
      Fake_helper.queue_peek_from f "i32" (threes 3);
      check_str "the command line's #1" "0x1008" (first_addr (call d "results" [] ~opts:[ ("near", "0x1008") ]));
      (* the window's poll: page 1 of the plain list, unrecorded, whose #1 is 0x1000 *)
      Fake_helper.queue_peek f "i32" (threes 3);
      check_str "the window's #1" "0x1000" (first_addr (unrecorded d "results" [] ~opts:[ ("from", "1"); ("n", "200") ]));
      Fake_helper.queue_write f ~prev:(Vint 3L);
      Fake_helper.queue_read f (Vint 9L);
      check_str "set 1 9" "set #1 (0x1008) to 9 (was 3)" (text (call d "set" [ "1"; "9" ]));
      check_bool "written where the command line was shown #1" true (Fake_helper.last_write f = (0x1008L, "i32", Put_value (Vint 9L)));
      (* the window reads another page: the command line's #3 is still there *)
      Fake_helper.queue_peek f "i32" [ List.nth (threes 3) 1 ];
      ignore (json (unrecorded d "results" [] ~opts:[ ("from", "2"); ("n", "1") ]));
      Fake_helper.queue_write f ~prev:(Vint 3L);
      Fake_helper.queue_read f (Vint 9L);
      check_str "set 3 9" "set #3 (0x1000) to 9 (was 3)" (text (call d "set" [ "3"; "9" ]));
      check_bool "written at its #3" true (Fake_helper.last_write f = (0x1000L, "i32", Put_value (Vint 9L))))

(* with --at, row n is found afresh in the hunt as results lists it without --near (--all as
   given); the rows a listing left are neither used nor changed *)
let a_row_found_afresh () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      ignore (text (attach d));
      Fake_helper.queue_scan f [ gi "i32" 3; gi "u8" 1 ];
      ignore (text (call d "find" [ "3" ]));
      let at a = [ ("at", a) ] and all = [ ("all", "true") ] in
      let three a = { addr = a; v = Some (Vint 3L) } in
      let wrote n = Fake_helper.queue_write f ~prev:(Vint 3L); Fake_helper.queue_read f (Vint n) in
      (* nothing listed yet: row 1 is the first whole number *)
      Fake_helper.queue_peek f "i32" [ three 0x1000L ];
      wrote 9L;
      check_str "set, nothing listed" "set #1 (0x1000) to 9 (was 3)" (text (call d "set" [ "1"; "9" ] ~opts:(at "0x1000")));
      check_bool "its row alone" true (Fake_helper.last_peek f "i32" = (0, 1));
      check_bool "written there" true (Fake_helper.last_write f = (0x1000L, "i32", Put_value (Vint 9L)));
      (* a recorded listing: its #1 is 0x1004 *)
      Fake_helper.queue_peek f "i32" [ three 0x1004L; three 0x1008L ];
      check_str "listed from 2" "0x1004" (first_addr (call d "results" [] ~opts:[ ("from", "2") ]));
      (* with --all the rarer forms count too, fewest matches first: row 1 is the 1-byte one *)
      Fake_helper.queue_peek f "u8" [ three 0x2001L ];
      wrote 9L;
      check_str "set, all" "set #1 (0x2001) to 9 (was 3)" (text (call d "set" [ "1"; "9" ] ~opts:(at "0x2001" @ all)));
      check_bool "the 1-byte one" true (Fake_helper.last_write f = (0x2001L, "u8", Put_value (Vint 9L)));
      Fake_helper.queue_peek f "i32" [ three 0x1000L ];
      Fake_helper.queue_read f (Vint 9L);
      check_str "keep, all" "kept #2 (0x1000) as hp (9, whole number)" (text (call d "keep" [ "2"; "hp" ] ~opts:(at "0x1000" @ all)));
      check_bool "row 2 is the first whole number" true (Fake_helper.last_peek f "i32" = (0, 1));
      (* past the end: no row, and nothing asked of the helper *)
      let calls = List.length (Fake_helper.calls f) in
      check_err "no row 5" "no result #5. run: moth results" (call d "set" [ "5"; "9" ] ~opts:(at "0x1000"));
      check_err "nor kept" "no result #5. run: moth results" (call d "keep" [ "5" ] ~opts:(at "0x1000"));
      check_int "nothing asked" calls (List.length (Fake_helper.calls f));
      (* without --at, the recorded listing's rows, as they were *)
      wrote 7L;
      check_str "set 2 7" "set #2 (0x1004) to 7 (was 3)" (text (call d "set" [ "2"; "7" ]));
      check_bool "the listing's #2" true (Fake_helper.last_write f = (0x1004L, "i32", Put_value (Vint 7L)));
      check_err "the listing has no #1" "no result #1. run: moth results" (call d "set" [ "1"; "7" ]))

(* --kind pins the row's interpretation as well: the same address read as another kind is not
   that row *)
let a_row_of_another_kind () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      hunt_for_three d f 3;
      let pin kind = [ ("at", "0x1000"); ("kind", kind) ] in
      Fake_helper.queue_peek f "i32" (threes 1);
      check_err "set as a decimal" (moved_text 1 "0x1000") (call d "set" [ "1"; "9" ] ~opts:(pin "f32"));
      Fake_helper.queue_peek f "i32" (threes 1);
      check_err "keep as a decimal" (moved_text 1 "0x1000") (call d "keep" [ "1"; "hp" ] ~opts:(pin "f32"));
      check_int "nothing written" 0 (Fake_helper.write_calls f);
      check_str "nothing kept" nothing_kept (text (call d "kept" []));
      Fake_helper.queue_peek f "i32" (threes 1);
      Fake_helper.queue_write f ~prev:(Vint 3L);
      Fake_helper.queue_read f (Vint 9L);
      check_str "as its own kind" "set #1 (0x1000) to 9 (was 3)" (text (call d "set" [ "1"; "9" ] ~opts:(pin "i32")));
      check_bool "written" true (Fake_helper.last_write f = (0x1000L, "i32", Put_value (Vint 9L))))

let kind_and_all_go_with_at () =
  let check n w g = Alcotest.(check (result unit string)) n w g in
  let u cmd args opts = Daemon.check_usage ~cmd ~args ~opts in
  let kind = Error "--kind goes with --at" and all = Error "--all goes with --at" in
  check "set --kind" kind (u "set" [ "1"; "9" ] [ ("kind", "i32") ]);
  check "keep --kind" kind (u "keep" [ "1"; "hp" ] [ ("kind", "i32") ]);
  check "set by name --kind" kind (u "set" [ "hp"; "9" ] [ ("kind", "i32") ]);
  check "results --kind" kind (u "results" [] [ ("kind", "i32") ]);
  check "set --all" all (u "set" [ "1"; "9" ] [ ("all", "true") ]);
  check "keep --all" all (u "keep" [ "1" ] [ ("all", "true") ]);
  check "set --at --kind --all" (Ok ()) (u "set" [ "1"; "9" ] [ ("at", "0x10"); ("kind", "i32"); ("all", "true") ]);
  check "keep --at --kind --all" (Ok ()) (u "keep" [ "1"; "hp" ] [ ("at", "0x10"); ("kind", "i32"); ("all", "true") ]);
  check "results --all" (Ok ()) (u "results" [] [ ("all", "true") ]);
  let fk, sh = fake () in
  with_daemon sh (fun d ->
      hunt_for_three d fk.script 3;
      check_err "from the daemon, --kind" "--kind goes with --at" (call d "set" [ "1"; "9" ] ~opts:[ ("kind", "i32") ]);
      check_err "from the daemon, --all" "--all goes with --at" (call d "keep" [ "1" ] ~opts:[ ("all", "true") ]);
      check_int "nothing asked of the helper" 0 (Fake_helper.write_calls fk.script + List.length (Fake_helper.peeks fk.script)))

let usage () =
  let check n w g = Alcotest.(check (result unit string)) n w g in
  check "unknown" (Error "unknown command: frob. run: moth help") (Daemon.check_usage ~cmd:"frob" ~args:[] ~opts:[]);
  check "find" (Ok ()) (Daemon.check_usage ~cmd:"find" ~args:[ "1" ] ~opts:[]);
  check "next" (Error "next needs a number, or one of: down, up, same, changed") (Daemon.check_usage ~cmd:"next" ~args:[] ~opts:[]);
  check "set" (Ok ()) (Daemon.check_usage ~cmd:"set" ~args:[ "#3"; "-5" ] ~opts:[]);
  check "log" (Ok ()) (Daemon.check_usage ~cmd:"log" ~args:[] ~opts:[ ("n", "50") ]);
  check "watch" (Ok ()) (Daemon.check_usage ~cmd:"watch" ~args:[] ~opts:[]);
  check "watch --after 0" (Ok ()) (Daemon.check_usage ~cmd:"watch" ~args:[] ~opts:[ ("after", "0") ]);
  check "watch takes nothing" (Error "watch takes no arguments") (Daemon.check_usage ~cmd:"watch" ~args:[ "3" ] ~opts:[]);
  List.iter
    (fun v -> check ("watch --after " ^ v) (Error "watch --after needs an event number") (Daemon.check_usage ~cmd:"watch" ~args:[] ~opts:[ ("after", v) ]))
    [ "x"; "-1"; ""; "1.5"; "99999999999999999999" ];
  let _, sh = fake () in
  with_daemon sh (fun d ->
      check_err "from the daemon" "unknown command: frob. run: moth help" (call d "frob" []);
      check_err "watch, from the daemon" "watch --after needs an event number" (request d [ ("cmd", `String "watch"); ("opts", `Assoc [ ("after", `String "x") ]) ]))

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
  same "after" (inv "watch" [] [ ("after", "12") ] true) (p [ "watch"; "--after"; "12"; "--json" ]);
  same "after needs a value" (Error "--after needs a value") (p [ "watch"; "--after" ]);
  same "at" (inv "set" [ "3"; "9" ] [ ("at", "0x10") ] false) (p [ "set"; "3"; "9"; "--at"; "0x10" ]);
  same "at needs a value" (Error "--at needs a value") (p [ "keep"; "3"; "--at" ]);
  same "at, kind and all" (inv "set" [ "3"; "9" ] [ ("at", "0x10"); ("kind", "i32"); ("all", "true") ] false) (p [ "set"; "3"; "9"; "--at"; "0x10"; "--kind"; "i32"; "--all" ]);
  same "kind needs a value" (Error "--kind needs a value") (p [ "set"; "3"; "9"; "--kind" ]);
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

(* ---- launch and games ---- *)

let path_of_file () =
  let dir = Filename.concat (fresh_home ()) "g" in
  let rec mk d = if not (Sys.file_exists d) then (mk (Filename.dirname d); Unix.mkdir d 0o755) in
  mk dir;
  dir

let touch dir name =
  let p = Filename.concat dir name in
  close_out (open_out p);
  p

let nothing_yet = "nothing launched from moth yet. start a game with: moth launch --bottle <bottle> <full path to game.exe>"

let launching () =
  let _, sh = fake () in
  let launch, calls = fake_launch () in
  let dir = path_of_file () in
  Fun.protect ~finally:(fun () -> rm (Filename.dirname dir)) (fun () ->
      let game = touch dir "game.exe" in
      with_daemon ~launch sh (fun d ->
          check_str "empty" nothing_yet (text (call d "games" []));
          let r = call d "launch" [ game; "-windowed" ] ~opts:[ ("bottle", "uhmm") ] in
          check_str "started" (Printf.sprintf "started game.exe in uhmm. when it is up: moth attach --bottle \"uhmm\" game.exe") (text r);
          check_json "json"
            (`Assoc [ ("message", `String (text r)); ("name", `String "game.exe"); ("bottle", `String "uhmm"); ("path", `String game) ])
            (json r);
          Alcotest.(check (list (triple string string (list string)))) "called once" [ ("uhmm", game, [ "-windowed" ]) ] !calls;
          check_str "games" (Printf.sprintf "game.exe  uhmm  %s" game) (text (call d "games" []));
          check_json "games json"
            (`List [ `Assoc [ ("name", `String "game.exe"); ("bottle", `String "uhmm"); ("path", `String game) ] ])
            (json (call d "games" []));
          check_bool "games.json in the home" true (Sys.file_exists (Filename.concat d.home "games.json"));
          (* an unrecorded games is fine *)
          ignore (Client.call ~autostart:false ~home:d.home ~actor:"user" ~cmd:"games" ~args:[] ~opts:[] ())))

let launch_remembers_once () =
  let _, sh = fake () in
  let launch, calls = fake_launch () in
  let dir = path_of_file () in
  Fun.protect ~finally:(fun () -> rm (Filename.dirname dir)) (fun () ->
      let game = touch dir "game.exe" in
      with_daemon ~launch sh (fun d ->
          let go () = ignore (call d "launch" [ game ] ~opts:[ ("bottle", "uhmm") ]) in
          go ();
          go ();
          check_int "launched twice" 2 (List.length !calls);
          check_int "remembered once" 1 (List.length (Yojson.Safe.Util.to_list (json (call d "games" []))));
          (* another bottle is another entry, newest first *)
          ignore (call d "launch" [ game ] ~opts:[ ("bottle", "other") ]);
          check_strs "newest first" [ "other"; "uhmm" ]
            (List.map (fun g -> Yojson.Safe.Util.(member "bottle" g |> to_string)) (Yojson.Safe.Util.to_list (json (call d "games" []))));
          (* 21 different ones leave 20, the oldest gone *)
          for i = 1 to 21 do
            ignore (call d "launch" [ touch dir (Printf.sprintf "g%d.exe" i) ] ~opts:[ ("bottle", "uhmm") ])
          done;
          let names = List.map (fun g -> Yojson.Safe.Util.(member "name" g |> to_string)) (Yojson.Safe.Util.to_list (json (call d "games" []))) in
          check_int "twenty" 20 (List.length names);
          check_str "newest" "g21.exe" (List.hd names);
          check_str "oldest kept" "g2.exe" (List.nth names 19)))

let launch_refusals () =
  let _, sh = fake () in
  let launch, calls = fake_launch () in
  let dir = path_of_file () in
  Fun.protect ~finally:(fun () -> rm (Filename.dirname dir)) (fun () ->
      let txt = touch dir "notes.txt" in
      let upper = touch dir "GAME.EXE" in
      with_daemon ~launch sh (fun d ->
          let b = [ ("bottle", "uhmm") ] in
          let usage = "launch needs a bottle and a program: moth launch --bottle <bottle> <full path to game.exe>" in
          check_err "no bottle" usage (call d "launch" [ "/x/game.exe" ]);
          check_err "no program" usage (call d "launch" [] ~opts:b);
          check_err "relative" "launch needs the full path to a windows program (.exe): game.exe" (call d "launch" [ "game.exe" ] ~opts:b);
          check_err "txt" (Printf.sprintf "launch needs the full path to a windows program (.exe): %s" txt) (call d "launch" [ txt ] ~opts:b);
          check_err "missing" (Printf.sprintf "no such file: %s/nope.exe" dir) (call d "launch" [ dir ^ "/nope.exe" ] ~opts:b);
          check_int "never called" 0 (List.length !calls);
          ignore (text (call d "launch" [ upper ] ~opts:b));
          check_int ".EXE counts" 1 (List.length !calls);))

let launch_fails () =
  let _, sh = fake () in
  let launch, _ = fake_launch ~result:(Error "no bottle named x") () in
  let dir = path_of_file () in
  Fun.protect ~finally:(fun () -> rm (Filename.dirname dir)) (fun () ->
      let game = touch dir "game.exe" in
      with_daemon ~launch sh (fun d ->
          check_err "its message" "no bottle named x" (call d "launch" [ game ] ~opts:[ ("bottle", "x") ]);
          check_str "not remembered" nothing_yet (text (call d "games" []))))

let games_corrupt () =
  let _, sh = fake () in
  let home = fresh_home () in
  Unix.mkdir home 0o700;
  let oc = open_out (Filename.concat home "games.json") in
  output_string oc "{not json";
  close_out oc;
  with_daemon ~home sh (fun d -> check_str "empty" nothing_yet (text (call d "games" [])))

let launch_args_pass_through () =
  let show = function
    | Ok { Client.cmd; args; opts; json } ->
        Printf.sprintf "%s [%s] {%s} %b" cmd (String.concat "; " args) (String.concat "; " (List.map (fun (k, v) -> k ^ "=" ^ v) opts)) json
    | Error e -> "error " ^ e
  in
  let p = Client.parse_args in
  check_str "after the path" "launch [/g/a.exe; -windowed; --json; --bottle; x; -n] {bottle=uhmm} false"
    (show (p [ "launch"; "--bottle"; "uhmm"; "/g/a.exe"; "-windowed"; "--json"; "--bottle"; "x"; "-n" ]));
  check_str "path first" "launch [/g/a.exe; -windowed] {} false" (show (p [ "launch"; "/g/a.exe"; "-windowed" ]));
  check_str "json before the path is moth's" "launch [/g/a.exe] {bottle=u} true" (show (p [ "launch"; "--json"; "--bottle"; "u"; "/g/a.exe" ]));
  check_str "no path" "launch [] {bottle=u} false" (show (p [ "launch"; "--bottle"; "u" ]))

(* ---- what the agent may not do through moth ---- *)

let needs_at = "give the row's address too, so the write cannot land on another row: add --at <the address moth results showed for it>"

(* the agent's set and keep by row number need --at; by name, by address and the person's own
   are as before *)
let the_agents_row_writes_carry_at () =
  let fk, sh = fake () in
  let f = fk.script in
  with_daemon sh (fun d ->
      hunt_for_three d f 3;
      Fake_helper.queue_peek f "i32" (threes 3);
      ignore (text (call d "results" []));
      let agent ?(opts = []) cmd args = call ~actor:"agent" d cmd args ~opts in
      let asked = List.length (Fake_helper.calls f) in
      check_err "set 1" needs_at (agent "set" [ "1"; "9" ]);
      check_err "set #1" needs_at (agent "set" [ "#1"; "9" ]);
      check_err "keep 1" needs_at (agent "keep" [ "1" ]);
      check_err "keep 1 hp" needs_at (agent "keep" [ "1"; "hp" ]);
      check_int "nothing asked of the helper" asked (List.length (Fake_helper.calls f));
      check_str "nothing kept" nothing_kept (text (call d "kept" []));
      let refused = List.filter (fun ev -> Yojson.Safe.Util.member "actor" ev = `String "agent") (stored d) in
      check_int "each recorded as the agent's" 4 (List.length refused);
      check_bool "as failures" true (List.for_all (fun ev -> Yojson.Safe.Util.member "ok" ev = `Bool false) refused);
      (* with --at: written and kept *)
      Fake_helper.queue_peek f "i32" (threes 1);
      Fake_helper.queue_write f ~prev:(Vint 3L);
      Fake_helper.queue_read f (Vint 9L);
      check_str "set 1 --at" "set #1 (0x1000) to 9 (was 3)" (text (agent "set" [ "1"; "9" ] ~opts:[ ("at", "0x1000") ]));
      Fake_helper.queue_peek f "i32" (threes ~base:0x1004 1);
      Fake_helper.queue_read f (Vint 3L);
      check_str "keep 2 --at" "kept #2 (0x1004) as hp (3, whole number)" (text (agent "keep" [ "2"; "hp" ] ~opts:[ ("at", "0x1004") ]));
      (* by name and by address, as before *)
      Fake_helper.queue_write f ~prev:(Vint 3L);
      Fake_helper.queue_read f (Vint 8L);
      check_str "set by name" "set hp to 8 (was 3)" (text (agent "set" [ "hp"; "8" ]));
      Fake_helper.queue_read f (Vint 3L);
      check_str "keep by address" "kept 0x1008 as mp (3, whole number)" (text (agent "keep" [ "0x1008"; "3"; "mp" ]));
      (* the person's row writes need no --at *)
      Fake_helper.queue_write f ~prev:(Vint 3L);
      Fake_helper.queue_read f (Vint 7L);
      check_str "the person's set 3" "set #3 (0x1008) to 7 (was 3)" (text (call d "set" [ "3"; "7" ])))

let refused_launch = "the agent can only start a game you have started from moth before. start it yourself once: moth launch --bottle <bottle> <path>"

(* the agent starts only a game the person started from moth, in that bottle, with no arguments
   of its own *)
let the_agent_starts_only_a_known_game () =
  let _, sh = fake () in
  let launch, calls = fake_launch () in
  let dir = path_of_file () in
  Fun.protect ~finally:(fun () -> rm (Filename.dirname dir)) (fun () ->
      let game = touch dir "game.exe" and other = touch dir "cmd.exe" in
      with_daemon ~launch sh (fun d ->
          let b = [ ("bottle", "uhmm") ] in
          let agent opts args = call ~actor:"agent" d "launch" args ~opts in
          check_err "nothing started from moth yet" refused_launch (agent b [ game ]);
          ignore (text (call d "launch" [ game; "-windowed" ] ~opts:b));
          check_err "another program" refused_launch (agent b [ other; "/c"; "anything" ]);
          check_err "another bottle" refused_launch (agent [ ("bottle", "other") ] [ game ]);
          check_err "arguments of its own" "the agent starts a game as you started it, with no arguments of its own" (agent b [ game; "-windowed" ]);
          check_int "only the person's start ran" 1 (List.length !calls);
          check_str "the game, as listed" "started game.exe in uhmm. when it is up: moth attach --bottle \"uhmm\" game.exe" (text (agent b [ game ]));
          Alcotest.(check (list (triple string string (list string)))) "started with no arguments" [ ("uhmm", game, []); ("uhmm", game, [ "-windowed" ]) ] !calls;
          let refused = List.filter (fun ev -> Yojson.Safe.Util.(member "actor" ev = `String "agent" && member "ok" ev = `Bool false)) (stored d) in
          check_int "the refusals are recorded as the agent's" 4 (List.length refused)))

(* quiet is the person's switch: the agent may ask which it is, not turn it *)
let quiet_is_the_persons () =
  let _, sh = fake () in
  with_daemon sh (fun d ->
      let agent args = call ~actor:"agent" d "quiet" args in
      check_err "on" "quiet is the person's switch" (agent [ "on" ]);
      check_bool "not kept" false (Sys.file_exists (Filename.concat d.home "quiet"));
      check_str "it may ask" "quiet is off" (text (agent []));
      ignore (text (call d "quiet" [ "on" ]));
      check_err "off" "quiet is the person's switch" (agent [ "off" ]);
      check_str "still on" "quiet is on" (text (agent []));
      check_json "status says so" (`Bool true) Yojson.Safe.Util.(member "quiet" (json (call d "status" [])));
      check_str "the person's off" "the agent is told what you do again" (text (call d "quiet" [ "off" ])))

let home () =
  with_env "MOTH_HOME" "/x/y" (fun () -> check_str "env" "/x/y" (Client.home ()));
  with_env "MOTH_HOME" "rel" (fun () -> check_str "made absolute" (Filename.concat (Sys.getcwd ()) "rel") (Client.home ()));
  with_env "MOTH_HOME" "" (fun () ->
      with_env "HOME" "/Users/u" (fun () -> check_str "default" "/Users/u/Library/Application Support/moth" (Client.home ())))

(* ---- quiet, and moth wait ---- *)

let quiet_is_kept () =
  let _, sh = fake () in
  let quiet d = Yojson.Safe.Util.(json (call d "status" []) |> member "quiet") in
  with_daemon sh (fun d ->
      check_json "off to begin with" (`Bool false) (quiet d);
      check_str "no argument" "quiet is off" (text (call d "quiet" []));
      check_str "on" "quiet: the agent is not told what you do. it can still look: moth log" (text (call d "quiet" [ "on" ]));
      check_str "no argument, on" "quiet is on" (text (call d "quiet" []));
      check_json "status says so" (`Bool true) (quiet d);
      check_err "neither" "quiet takes on or off" (call d "quiet" [ "maybe" ]);
      check_err "two words" "quiet takes on or off" (call d "quiet" [ "on"; "off" ]);
      check_json "still on" (`Bool true) (quiet d);
      let refused = List.filter (fun ev -> cmd_of ev = "quiet" && not Yojson.Safe.Util.(member "ok" ev |> to_bool)) (stored d) in
      check_int "both refusals are events" 2 (List.length refused);
      check_bool "that acted" true (List.for_all act_of refused);
      ignore (call d "stop" []);
      Thread.join d.thread;
      check_bool "the file says on" true (Sys.file_exists (Filename.concat d.home "quiet"));
      let d = start ~home:d.home sh in
      check_json "a new daemon on the same home is still quiet" (`Bool true) (quiet d);
      check_str "off" "the agent is told what you do again" (text (call d "quiet" [ "off" ]));
      check_bool "the file is gone" false (Sys.file_exists (Filename.concat d.home "quiet"));
      check_json "off" (`Bool false) (quiet d);
      ignore (call d "stop" []);
      Thread.join d.thread)

(* the real moth program, run as the agent's hook runs it: next to the test in _build *)
let moth_exe = lazy (Filename.concat (Sys.getcwd ()) "../bin/main.exe")

type waiter = { pid : int; err : string; mutable code : int option }

let spawned = ref []
let spawn_count = ref 0

(* [moth <args>] against [home] (and [env] besides), stdin and stdout on /dev/null, stderr in a file *)
let spawn ?(env = []) ~home args =
  incr spawn_count;
  let err = Printf.sprintf "/tmp/moth-wait-%d-%d.err" (Unix.getpid ()) !spawn_count in
  let null = Unix.openfile "/dev/null" [ Unix.O_RDWR ] 0 in
  let errfd = Unix.openfile err [ Unix.O_WRONLY; Unix.O_CREAT; Unix.O_TRUNC ] 0o600 in
  let exe = Lazy.force moth_exe in
  let own = List.filter (fun e -> not (String.starts_with ~prefix:"MOTH_HOME=" e)) (Array.to_list (Unix.environment ())) in
  let env = Array.of_list (("MOTH_HOME=" ^ home) :: (List.map (fun (k, v) -> k ^ "=" ^ v) env @ own)) in
  let pid = Unix.create_process_env exe (Array.of_list (exe :: args)) env null null errfd in
  Unix.close null;
  Unix.close errfd;
  let w = { pid; err; code = None } in
  spawned := w :: !spawned;
  w

let spawn_wait_with env ~home ~session = spawn ~env ~home [ "wait"; "--session"; session ]
let spawn_wait = spawn_wait_with []

let reap w =
  if w.code = None then
    (match Unix.waitpid [ Unix.WNOHANG ] w.pid with 0, _ -> () | _, Unix.WEXITED n -> w.code <- Some n | _, _ -> w.code <- Some (-1));
  w.code

(* its exit code, waiting up to [within] seconds; None when it is still running *)
let exit_within w within =
  let deadline = Unix.gettimeofday () +. within in
  let rec go () =
    match reap w with
    | Some n -> Some n
    | None when Unix.gettimeofday () >= deadline -> None
    | None ->
        Thread.delay 0.05;
        go ()
  in
  go ()

let read_all path = try In_channel.with_open_bin path In_channel.input_all with Sys_error _ -> ""

(* whether [f] comes true within [within] seconds *)
let until ?(within = 5.) f =
  let deadline = Unix.gettimeofday () +. within in
  let rec go () =
    f () || (Unix.gettimeofday () < deadline && (Thread.delay 0.05; go ()))
  in
  go ()

(* the waiter is watching once it has written its cursor (it does so as soon as the watch is up
   when there was none) *)
let watching dir = until (fun () -> Sys.file_exists (Filename.concat dir "cursor"))

let put_waiters_away () =
  List.iter
    (fun w ->
      (match reap w with
      | None -> (
          (try Unix.kill w.pid Sys.sigkill with Unix.Unix_error _ -> ());
          try ignore (Unix.waitpid [] w.pid) with Unix.Unix_error _ -> ())
      | Some _ -> ());
      try Sys.remove w.err with Sys_error _ -> ())
    !spawned;
  spawned := []

let () = before_daemon_stops := put_waiters_away

(* whatever a test started is gone when it ends, whatever happened *)
let with_waiters f = Fun.protect f ~finally:put_waiters_away

let spaced_home () = fresh_home () ^ " with a space"

(* [home]/agent/<name>, which is this test's pid unless given: a session that is alive *)
let session_dir ?(name = string_of_int (Unix.getpid ())) home =
  let mk d = try Unix.mkdir d 0o700 with Unix.Unix_error (Unix.EEXIST, _, _) -> () in
  mk home;
  mk (Filename.concat home "agent");
  let dir = Filename.concat (Filename.concat home "agent") name in
  mk dir;
  dir

let newest_seq d = Yojson.Safe.Util.(member "seq" (List.nth (stored d) (List.length (stored d) - 1)) |> to_int)
let write_cursor dir text = Out_channel.with_open_bin (Filename.concat dir "cursor") (fun oc -> output_string oc text)
let heading = "[moth] the person did this in moth:\n"

(* a home that is removed again however the test ends *)
let in_spaced_home f =
  let home = spaced_home () in
  Fun.protect ~finally:(fun () -> rm home) (fun () -> f home)

let wait_tells_the_agent () =
  let fk, sh = fake () in
  with_waiters (fun () ->
      with_daemon ~home:(spaced_home ()) sh (fun d ->
          ignore (text (attach d));
          let dir = session_dir d.home in
          let w = spawn_wait ~home:d.home ~session:dir in
          check_bool "it is watching" true (watching dir);
          Fake_helper.queue_scan fk.script [ gi "i32" 3 ];
          ignore (text (call d "find" [ "3" ]));
          check_bool "exits 2 within 4 seconds" true (exit_within w 4. = Some 2);
          let err = read_all w.err in
          check_bool "tells what the person did" true (String.starts_with ~prefix:(heading ^ "- find 3: ") err);
          check_str "the cursor is that event's seq" (string_of_int (newest_seq d)) (String.trim (read_all (Filename.concat dir "cursor")));
          let mode f = (Unix.stat (Filename.concat dir f)).Unix.st_perm land 0o777 in
          check_int "the cursor is the owner's alone" 0o600 (mode "cursor");
          check_int "so is the waiter file" 0o600 (mode "waiter");
          (* the next waiter starts after it, and has nothing to say *)
          let again = spawn_wait ~home:d.home ~session:dir in
          check_bool "still waiting after 3 seconds" true (exit_within again 3. = None)))

let two_waiters () =
  let fk, sh = fake () in
  with_waiters (fun () ->
      with_daemon ~home:(spaced_home ()) sh (fun d ->
          ignore (text (attach d));
          let dir = session_dir d.home in
          let first = spawn_wait ~home:d.home ~session:dir in
          check_bool "the first is watching" true (watching dir);
          let second = spawn_wait ~home:d.home ~session:dir in
          (* the older one notices on its next tick; the newer one starts from the cursor file, so
             it hears the find whenever it connects *)
          check_bool "the older one leaves quietly" true (exit_within first 3. = Some 0);
          Fake_helper.queue_scan fk.script [ gi "i32" 3 ];
          ignore (text (call d "find" [ "3" ]));
          check_bool "the newer one tells" true (exit_within second 4. = Some 2);
          check_str "the older said nothing" "" (read_all first.err)))

(* the older waiter holds a find when a newer one takes over: the note comes once, from the newer *)
let a_take_over_holding_an_event () =
  let fk, sh = fake () in
  with_waiters (fun () ->
      with_daemon ~home:(spaced_home ()) sh (fun d ->
          ignore (text (attach d));
          let dir = session_dir d.home in
          let first = spawn_wait ~home:d.home ~session:dir in
          check_bool "the first is watching" true (watching dir);
          Fake_helper.queue_scan fk.script [ gi "i32" 3 ];
          ignore (text (call d "find" [ "3" ]));
          let second = spawn_wait ~home:d.home ~session:dir in
          check_bool "the older one leaves without telling" true (exit_within first 3. = Some 0);
          check_str "saying nothing" "" (read_all first.err);
          check_bool "the newer one tells" true (exit_within second 4. = Some 2);
          check_bool "what the older one held" true (String.starts_with ~prefix:(heading ^ "- find 3: ") (read_all second.err))))

let a_dead_session () =
  in_spaced_home (fun home ->
      with_waiters (fun () ->
          (* a pid that has been and gone *)
          let null = Unix.openfile "/dev/null" [ Unix.O_RDWR ] 0 in
          let gone = Unix.create_process "true" [| "true" |] null null null in
          ignore (Unix.waitpid [] gone);
          Unix.close null;
          let dir = session_dir ~name:(string_of_int gone) home in
          let w = spawn_wait ~home ~session:dir in
          check_bool "exits 0 within 2 seconds" true (exit_within w 2. = Some 0);
          check_str "and says nothing" "" (read_all w.err);
          check_bool "and wrote nothing there" false (Sys.file_exists (Filename.concat dir "waiter"))))

(* folder names that are no process id: kill would take 0 and -1 for whole groups of them *)
let names_that_are_no_pid () =
  in_spaced_home (fun home ->
      with_waiters (fun () ->
          List.iter
            (fun name ->
              let w = spawn_wait ~home ~session:(session_dir ~name home) in
              check_bool (name ^ " exits 0 within 2 seconds") true (exit_within w 2. = Some 0);
              check_str (name ^ " says nothing") "" (read_all w.err))
            [ "0"; "-1"; "0x1f"; "12abc"; "agent" ]))

let wait_with_no_daemon () =
  let fk, sh = fake () in
  in_spaced_home (fun home ->
      with_waiters (fun () ->
          let dir = session_dir home in
          let w = spawn_wait ~home ~session:dir in
          check_bool "still waiting after 3 seconds" true (exit_within w 3. = None);
          check_bool "and started no daemon" false (Sys.file_exists (Filename.concat home "core.sock"));
          with_daemon ~home sh (fun d ->
              ignore (text (attach d));
              check_bool "it finds the service" true (watching dir);
              Fake_helper.queue_scan fk.script [ gi "i32" 3 ];
              ignore (text (call d "find" [ "3" ]));
              check_bool "exits 2 once the person acts" true (exit_within w 4. = Some 2);
              check_bool "with the note" true (String.starts_with ~prefix:heading (read_all w.err)))))

(* the person's own stop: its event is held when the feed closes, and is told with no service up *)
let wait_tells_a_stop () =
  let _, sh = fake () in
  with_waiters (fun () ->
      with_daemon ~home:(spaced_home ()) sh (fun d ->
          let dir = session_dir d.home in
          let w = spawn_wait ~home:d.home ~session:dir in
          check_bool "it is watching" true (watching dir);
          ignore (call d "stop" []);
          Thread.join d.thread;
          check_bool "exits 2 within 4 seconds" true (exit_within w 4. = Some 2);
          check_str "with the stop in its note" (heading ^ "- stop: stopped\n") (read_all w.err);
          check_bool "and no service started" false (Sys.file_exists (sock d.home))))

(* the waiter's own look finds the game closed: recorded, and told *)
let wait_hears_the_game_closed ~cursor_file () =
  let fk, sh = fake () in
  with_waiters (fun () ->
      with_daemon ~home:(spaced_home ()) sh (fun d ->
          ignore (text (attach d));
          let dir = session_dir d.home in
          if cursor_file then write_cursor dir (string_of_int (newest_seq d));
          fk.alive <- false;
          let w = spawn_wait ~home:d.home ~session:dir in
          check_bool "exits 2" true (exit_within w 4. = Some 2);
          check_str "with the closed game in its note" (heading ^ "- status: not attached. disco.exe closed.\n") (read_all w.err)))

let wait_needs_a_session () =
  in_spaced_home (fun home ->
      with_waiters (fun () ->
          let w = spawn ~home [ "wait" ] in
          check_bool "exits 1, not 2" true (exit_within w 2. = Some 1);
          check_str "saying how" "wait is run by the agent's hook: moth wait --session <dir>\n" (read_all w.err);
          let w = spawn ~home [ "wait"; "--bogus" ] in
          check_bool "a bad option exits 1 too" true (exit_within w 2. = Some 1)))

(* moth wait does not use MOTH_ACTOR *)
let wait_ignores_the_actor () =
  in_spaced_home (fun home ->
      with_waiters (fun () ->
          let w = spawn_wait_with [ ("MOTH_ACTOR", "nobody") ] ~home ~session:(session_dir home) in
          check_bool "still waiting" true (exit_within w 1.5 = None);
          check_str "and saying nothing" "" (read_all w.err)))

(* the quiet state is kept beside the cursor, and starts a waiter that replays from it *)
let quiet_travels_with_the_cursor () =
  let fk, sh = fake () in
  with_waiters (fun () ->
      with_daemon ~home:(spaced_home ()) sh (fun d ->
          ignore (text (attach d));
          let dir = session_dir d.home in
          (* a waiter gone, having left the cursor while quiet; what was done after it is not told *)
          write_cursor dir (string_of_int (newest_seq d) ^ " quiet");
          Fake_helper.queue_scan fk.script [ gi "i32" 3 ];
          ignore (text (call d "find" [ "3" ]));
          let w = spawn_wait ~home:d.home ~session:dir in
          check_bool "nothing to tell after 3 seconds" true (exit_within w 3. = None);
          Unix.kill w.pid Sys.sigkill;
          ignore (exit_within w 2.);
          (* and a waiter that sees quiet go on writes it *)
          Sys.remove (Filename.concat dir "cursor");
          let w = spawn_wait ~home:d.home ~session:dir in
          check_bool "it is watching" true (watching dir);
          ignore (text (call d "quiet" [ "on" ]));
          Fake_helper.queue_scan fk.script [ gi "i32" 3 ];
          ignore (text (call d "find" [ "3" ]));
          let want = string_of_int (newest_seq d) ^ " quiet" in
          check_bool "the cursor says quiet" true (until (fun () -> String.trim (read_all (Filename.concat dir "cursor")) = want));
          check_bool "and it told nothing" true (exit_within w 0.5 = None)))

(* a service that accepts and never answers does not keep the waiter past its session *)
let a_silent_service () =
  in_spaced_home (fun home ->
      with_waiters (fun () ->
          let null = Unix.openfile "/dev/null" [ Unix.O_RDWR ] 0 in
          let child = Unix.create_process "sleep" [| "sleep"; "30" |] null null null in
          Unix.close null;
          let dir = session_dir ~name:(string_of_int child) home in
          let listener = Unix.socket Unix.PF_UNIX Unix.SOCK_STREAM 0 in
          Fun.protect
            ~finally:(fun () ->
              Unix.close listener;
              try
                Unix.kill child Sys.sigkill;
                ignore (Unix.waitpid [] child)
              with Unix.Unix_error _ -> ())
            (fun () ->
              Unix.bind listener (Unix.ADDR_UNIX (sock home));
              Unix.listen listener 4;
              let w = spawn_wait_with [ ("MOTH_WAIT_TIMEOUT", "2") ] ~home ~session:dir in
              check_bool "waiting while its session lives" true (exit_within w 1. = None);
              Unix.kill child Sys.sigkill;
              ignore (Unix.waitpid [] child);
              check_bool "gone within the limit and two seconds" true (exit_within w 4. = Some 0))))

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
          t "status for the panels" status_for_panels;
          t "kept says its note" kept_says_its_note;
          t "launch" launching;
          t "launch remembers one per bottle and path, twenty at most" launch_remembers_once;
          t "launch refusals" launch_refusals;
          t "launch that fails" launch_fails;
          t "games with a corrupt games.json" games_corrupt;
          t "launch passes the game's arguments through" launch_args_pass_through;
          t "at guards a row" at_guards_a_row;
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
          t "snapshot hunt" a_snapshot_hunt;
          t "status of a snapshot hunt" status_of_a_snapshot_hunt;
          t "snapshot usage" snapshot_usage;
          t "snapshot too big" snapshot_too_big;
          t "kept shows a put back" kept_put_back;
          t "snapshot cap" snapshot_cap;
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
      ( "feed",
        [
          t "a watcher sees a find" a_watcher_sees_a_find;
          t "watch after an event" watching_after;
          t "watch after: the newest 1,000" watching_after_many;
          t "reads that leave no trace" reads_that_leave_no_trace;
          t "only reads go unrecorded" only_reads_go_unrecorded;
          t "a closed game is recorded" a_closed_game_is_recorded;
          t "an agent is named" an_agent_is_named;
          t "events say whether they acted" events_say_whether_they_acted;
          t "a closed game acts" a_closed_game_acts;
          t "Client.watch: started and idle" client_watch_hooks;
          t "status says its protocol" status_says_its_protocol;
          t "a moth from another build is refused" another_build_is_refused;
          t "unrecorded results, target_gone: recorded" (a_read_that_loses_the_game "results" (answers_next "peek" "target_gone") game_closed);
          t "unrecorded results, helper_lost: recorded" (a_read_that_loses_the_game "results" (answers_next "peek" "helper_lost") lost_contact);
          t "unrecorded kept, target_gone: recorded" (a_read_that_loses_the_game ~keeping:true "kept" (answers_next "read" "target_gone") game_closed);
          t "unrecorded kept, helper_lost: recorded" (a_read_that_loses_the_game ~keeping:true "kept" (answers_next "read" "helper_lost") lost_contact);
          t "unrecorded status, helper_lost: recorded" (a_read_that_loses_the_game "status" (fun fk -> fk.status_lost <- true) lost_contact);
          t "sixteen watchers" sixteen_watchers;
          t "a watcher that never reads" a_watcher_that_never_reads;
          t "Client.watch with no daemon" watch_with_no_daemon;
          t "watch until stop" watching_until_stop;
          t "quiet is kept" quiet_is_kept;
        ] );
      ( "row writes",
        [
          t "a poll leaves the command line's rows" a_poll_leaves_the_rows;
          t "--at finds its row afresh" a_row_found_afresh;
          t "--kind refuses a row of another kind" a_row_of_another_kind;
          t "--kind and --all go with --at" kind_and_all_go_with_at;
        ] );
      ( "the agent's limits",
        [
          t "its row writes carry --at" the_agents_row_writes_carry_at;
          t "it starts only a game the person started" the_agent_starts_only_a_known_game;
          t "quiet is the person's" quiet_is_the_persons;
        ] );
      ( "wait",
        [
          t "moth wait tells the agent, in a home with a space" wait_tells_the_agent;
          t "two waiters on one session: the newer tells" two_waiters;
          t "a session that is gone" a_dead_session;
          t "no daemon, then a daemon" wait_with_no_daemon;
          t "the game closed under its own look, with a cursor file" (wait_hears_the_game_closed ~cursor_file:true);
          t "the game closed under its own look, with none" (wait_hears_the_game_closed ~cursor_file:false);
          t "a take-over while an event is held" a_take_over_holding_an_event;
          t "folder names that are no pid" names_that_are_no_pid;
          t "a stop is told with no service up" wait_tells_a_stop;
          t "moth wait ignores MOTH_ACTOR" wait_ignores_the_actor;
          t "quiet travels with the cursor" quiet_travels_with_the_cursor;
          t "a silent service" a_silent_service;
          t "no session" wait_needs_a_session;
        ] );
      ("client", [ t "parse args" parse_args; t "parse args: results options" parse_results_args; t "home" home ]);
    ]
