open Moth_core

let ev ?(actor = "user") ?(act = true) ?(ok = true) ?(summary = "found 5 places") seq cmd args =
  `Assoc
    [
      ("event", `Assoc
        [
          ("seq", `Int seq);
          ("ts", `Float 0.);
          ("actor", `String actor);
          ("act", `Bool act);
          ("cmd", `String cmd);
          ("args", `List (List.map (fun a -> `String a) args));
          ("ok", `Bool ok);
          ("summary", `String summary);
        ]);
    ]

let busy cmd args = `Assoc [ ("busy", `Assoc [ ("actor", `String "user"); ("cmd", `String cmd); ("args", `List (List.map (fun a -> `String a) args)) ]) ]
let heading = "[moth] the person did this in moth:"
let fresh ?(cursor = 0) ?(quiet = false) () = Wait.start ~cursor ~quiet

let verdict =
  Alcotest.testable
    (fun ppf -> function
      | Wait.Keep_waiting -> Format.pp_print_string ppf "keep waiting"
      | Wait.Tell { notes; cursor } -> Format.fprintf ppf "tell %d %S" cursor notes)
    ( = )

let told notes cursor = Wait.Tell { notes; cursor }
let check_v n w g = Alcotest.check verdict n w g

let one_find () =
  let w = fresh () in
  Wait.line w ~now:0. (ev 1 "find" [ "100" ]);
  check_v "at 1.4" Wait.Keep_waiting (Wait.check w ~now:1.4);
  check_v "at 1.6" (told (heading ^ "\n- find 100: found 5 places") 1) (Wait.check w ~now:1.6);
  check_v "nothing held after" Wait.Keep_waiting (Wait.check w ~now:9.)

let three_together () =
  let w = fresh () in
  Wait.line w ~now:0. (ev 1 "find" [ "100" ]);
  Wait.line w ~now:0.5 (ev 2 "next" [ "101" ]);
  Wait.line w ~now:1.0 (ev 3 "next" [ "102" ]);
  check_v "before" Wait.Keep_waiting (Wait.check w ~now:2.4);
  check_v "together"
    (told (heading ^ "\n- find 100: found 5 places\n- next 101: found 5 places\n- next 102: found 5 places") 3)
    (Wait.check w ~now:2.5);
  check_v "once" Wait.Keep_waiting (Wait.check w ~now:3.)

let busy_holds () =
  let w = fresh () in
  Wait.line w ~now:0. (ev 1 "find" [ "100" ]);
  Wait.line w ~now:0.2 (busy "next" [ "101" ]);
  check_v "while busy" Wait.Keep_waiting (Wait.check w ~now:5.);
  Wait.line w ~now:6. (ev 2 "next" [ "101" ]);
  check_v "just after" Wait.Keep_waiting (Wait.check w ~now:7.4);
  match Wait.check w ~now:7.5 with
  | Wait.Tell { cursor = 2; _ } -> ()
  | v -> Alcotest.failf "expected a tell, got %a" (Alcotest.pp verdict) v

(* the ten seconds are not waited for while a busy line is: once its event has come, they are over *)
let busy_holds_past_ten_seconds () =
  let w = fresh () in
  Wait.line w ~now:0. (ev 1 "find" [ "100" ]);
  Wait.line w ~now:0.2 (busy "next" [ "101" ]);
  check_v "at ten seconds" Wait.Keep_waiting (Wait.check w ~now:10.);
  check_v "at twelve" Wait.Keep_waiting (Wait.check w ~now:12.);
  Wait.line w ~now:13. (ev 2 "next" [ "101" ]);
  match Wait.check w ~now:13. with
  | Wait.Tell { cursor = 2; _ } -> ()
  | v -> Alcotest.failf "expected a tell as its event came, got %a" (Alcotest.pp verdict) v

let ten_seconds () =
  let w = fresh () in
  for i = 0 to 10 do
    Wait.line w ~now:(float_of_int i) (ev (i + 1) "next" [ "1" ]);
    if i < 10 then check_v (Printf.sprintf "at %d" i) Wait.Keep_waiting (Wait.check w ~now:(float_of_int i))
  done;
  match Wait.check w ~now:10. with Wait.Tell { cursor = 11; _ } -> () | v -> Alcotest.failf "expected a tell at 10, got %a" (Alcotest.pp verdict) v

let never_told () =
  let w = fresh () in
  Wait.line w ~now:0. (ev ~actor:"agent" 1 "find" [ "3" ]);
  Wait.line w ~now:0. (ev ~act:false 2 "status" []);
  Wait.line w ~now:0. (ev 3 "quiet" [ "on" ]);
  check_v "nothing" Wait.Keep_waiting (Wait.check w ~now:20.);
  Alcotest.(check int) "cursor" 3 (Wait.cursor w)

let quiet_on_off () =
  let w = fresh () in
  Wait.line w ~now:0. (ev 1 "quiet" [ "on" ]);
  Wait.line w ~now:0. (ev 2 "find" [ "3" ]);
  check_v "quiet" Wait.Keep_waiting (Wait.check w ~now:20.);
  Alcotest.(check bool) "is quiet" true (Wait.quiet w);
  Wait.line w ~now:21. (ev 3 "quiet" [ "off" ]);
  Wait.line w ~now:22. (ev 4 "next" [ "4" ]);
  check_v "alone" (told (heading ^ "\n- next 4: found 5 places") 4) (Wait.check w ~now:24.)

let quiet_drops_what_is_held () =
  let w = fresh () in
  Wait.line w ~now:0. (ev 1 "find" [ "3" ]);
  Wait.line w ~now:0.5 (ev 2 "quiet" [ "on" ]);
  check_v "nothing to tell" Wait.Keep_waiting (Wait.check w ~now:3.);
  Alcotest.(check int) "cursor moved past it" 2 (Wait.cursor w);
  Wait.line w ~now:4. (ev 3 "quiet" [ "off" ]);
  Wait.line w ~now:5. (ev 4 "next" [ "4" ]);
  check_v "only the next" (told (heading ^ "\n- next 4: found 5 places") 4) (Wait.check w ~now:7.)

let bare_and_failed_quiet_change_nothing () =
  let w = fresh () in
  Wait.line w ~now:0. (ev 1 "quiet" []);
  Wait.line w ~now:0. (ev ~ok:false 2 "quiet" [ "maybe" ]);
  Alcotest.(check bool) "not quiet" false (Wait.quiet w);
  check_v "nothing told for them" Wait.Keep_waiting (Wait.check w ~now:20.);
  Alcotest.(check int) "cursor" 2 (Wait.cursor w);
  Wait.line w ~now:21. (ev 3 "find" [ "3" ]);
  check_v "still told" (told (heading ^ "\n- find 3: found 5 places") 3) (Wait.check w ~now:23.)

let quiet_from_the_agent_counts () =
  let w = fresh () in
  Wait.line w ~now:0. (ev ~actor:"agent" 1 "quiet" [ "on" ]);
  Alcotest.(check bool) "quiet" true (Wait.quiet w);
  Wait.line w ~now:0. (ev ~ok:false 2 "quiet" [ "off" ]);
  Wait.line w ~now:0. (ev 3 "quiet" []);
  Alcotest.(check bool) "failed or bare changes nothing" true (Wait.quiet w)

let failed () =
  let w = fresh () in
  Wait.line w ~now:0. (ev ~ok:false ~summary:"nothing to narrow" 1 "next" [ "5" ]);
  check_v "failed" (told (heading ^ "\n- next 5: failed: nothing to narrow") 1) (Wait.check w ~now:2.)

let many () =
  let w = fresh () in
  for i = 1 to 15 do
    Wait.line w ~now:0. (ev ~summary:(Printf.sprintf "s%d" i) i "next" [ string_of_int i ])
  done;
  match Wait.check w ~now:2. with
  | Wait.Tell { notes; cursor = 15 } ->
      let lines = String.split_on_char '\n' notes in
      Alcotest.(check int) "lines" 14 (List.length lines);
      Alcotest.(check string) "heading" heading (List.nth lines 0);
      Alcotest.(check string) "earlier" "- (3 earlier steps: moth log)" (List.nth lines 1);
      Alcotest.(check string) "oldest shown" "- next 4: s4" (List.nth lines 2);
      Alcotest.(check string) "newest" "- next 15: s15" (List.nth lines 13)
  | v -> Alcotest.failf "expected a tell, got %a" (Alcotest.pp verdict) v

let below_cursor () =
  let w = fresh ~cursor:5 () in
  Wait.line w ~now:0. (ev 4 "find" [ "3" ]);
  Wait.line w ~now:0. (ev 5 "find" [ "3" ]);
  check_v "ignored" Wait.Keep_waiting (Wait.check w ~now:20.);
  Alcotest.(check int) "cursor stays" 5 (Wait.cursor w);
  Wait.line w ~now:20. (ev 6 "find" [ "3" ]);
  check_v "told" (told (heading ^ "\n- find 3: found 5 places") 6) (Wait.check w ~now:22.)

let dropped_forgets_busy () =
  let w = fresh () in
  Wait.line w ~now:0. (ev 1 "find" [ "3" ]);
  Wait.line w ~now:0. (busy "next" [ "4" ]);
  Wait.dropped w;
  match Wait.check w ~now:2. with Wait.Tell _ -> () | v -> Alcotest.failf "expected a tell, got %a" (Alcotest.pp verdict) v

let () =
  let t n f = Alcotest.test_case n `Quick f in
  Alcotest.run "wait"
    [
      ( "bundling",
        [
          t "one event is told after a pause" one_find;
          t "three events are told together" three_together;
          t "a busy line holds the telling" busy_holds;
          t "a busy line holds past ten seconds" busy_holds_past_ten_seconds;
          t "a steady stream is told at ten seconds" ten_seconds;
          t "agent, reads and quiet are never told" never_told;
          t "quiet drops, and off replays nothing" quiet_on_off;
          t "going quiet drops what was held" quiet_drops_what_is_held;
          t "a bare or failed quiet changes nothing" bare_and_failed_quiet_change_nothing;
          t "quiet from anyone, only when it worked" quiet_from_the_agent_counts;
          t "a failure says so" failed;
          t "more than twelve are cut" many;
          t "events at or below the cursor are ignored" below_cursor;
          t "a dropped feed forgets busy lines" dropped_forgets_busy;
        ] );
    ]
