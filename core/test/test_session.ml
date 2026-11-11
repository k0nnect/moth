open Moth_core
open Helper_api
open Fake_helper

let ok = function Ok x -> x | Error e -> Alcotest.failf "unexpected error: %s" e
let check_int n w g = Alcotest.(check int) n w g
let check_bool n w g = Alcotest.(check bool) n w g
let check_str n w g = Alcotest.(check string) n w g
let check_ids n w g = Alcotest.(check (list string)) n w g

let show_pred = function
  | Eq n -> Printf.sprintf "eq %Ld" n
  | Range (a, b) -> Printf.sprintf "range %h %h" a b
  | Lt_prev -> "lt_prev"
  | Gt_prev -> "gt_prev"
  | Eq_prev -> "eq_prev"
  | Ne_prev -> "ne_prev"

let check_pred n w g = check_str n (show_pred w) (show_pred g)
let check_err n msg r = match r with Error e -> check_str n msg e | Ok _ -> Alcotest.failf "%s: expected the error %S" n msg
let gi ?(tracked = true) ?values id count : group_info = { id; count; tracked; has_values = Option.value values ~default:tracked }
let it addr v : item = { addr; v }

let fresh () =
  let h, f = Fake_helper.create () in
  (Session.create h, f)

let stats ?(bytes = 0) ?(lost = 0) ms = { zero_stats with bytes; lost; ms }
let ids_of (o : Session.outcome) = List.map (fun ((g : group_info), _) -> g.id) o.groups
let fell_back (o : Session.outcome) = List.exists (function Session.Fell_back _ -> true | _ -> false) o.notes
let closed = "the game closed. attach again once it's running."
let no_hunt = "no hunt in progress. start one with: moth find <number>"
let gone : err = { code = "target_gone"; message = "" }

let find_sends_plan () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 241178; gi "f32" 18382 ];
  let o = ok (Session.find s "100") in
  check_int "main" 259560 o.main;
  check_ids "scan groups" [ "i32"; "f32"; "i16"; "i64"; "f64"; "u8"; "i32x100"; "i32x10"; "i32x1000"; "f32pct"; "i32u"; "f32u" ] (last_scan_ids f);
  check_bool "keep" false (last_scan_keep f);
  check_bool "spec from the interpretation" true
    (last_scan_spec f "i32u" = ({ id = "i32u"; vtype = "i32"; align = 1; exclude_align = 4; pred = Eq 100L } : group_spec));
  check_pred "scaled" (Eq 10000L) (last_scan_spec f "i32x100").pred;
  check_bool "by tag" true (o.by_tag = [ ("whole number", 241178); ("decimal", 18382) ]);
  check_int "wide" 0 o.wide;
  check_bool "no notes" true (o.notes = []);
  check_bool "not a dead end" false o.dead_end;
  check_ids "groups" [ "i32"; "f32" ] (ids_of o);
  check_bool "tiers" true (List.map snd o.groups = [ Interp.Main; Interp.Main ])

let next_narrows () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 241178; gi "f32" 18382; gi "i32x100" 900 ];
  ignore (ok (Session.find s "100"));
  queue_narrow f ~applied:true [ gi "i32" 2; gi "f32" 1; gi "i32x100" 0 ];
  let o = ok (Session.next_number s "85") in
  check_int "main" 3 o.main;
  check_bool "atomic" true (last_narrow_atomic f);
  check_pred "i32x100" (Eq 8500L) (last_narrow_pred f "i32x100");
  check_pred "i32" (Eq 85L) (last_narrow_pred f "i32");
  check_pred "f32" (Range (84., 86.)) (last_narrow_pred f "f32");
  check_int "no rescan" 1 (scan_calls f);
  check_ids "nothing dropped" [] (dropped f);
  check_bool "no fallback" false (fell_back o);
  check_bool "by tag" true (o.by_tag = [ ("whole number", 2); ("decimal", 1) ])

let fallback_and_dead_end () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 241178; gi "f32" 18382 ];
  ignore (ok (Session.find s "100"));
  queue_narrow f ~applied:false [];
  queue_narrow f ~applied:true [ gi "i32" 4; gi "f32" 0 ];
  let o = ok (Session.next_number s "60") in
  check_bool "fell back" true (List.mem (Session.Fell_back Down) o.notes);
  check_pred "relation" Lt_prev (last_narrow_pred f "i32");
  check_bool "atomic" true (last_narrow_atomic f);
  check_int "main" 4 o.main;
  check_int "exact, then relation" 2 (narrow_calls f);
  (* dead end leaves the hunt as it was *)
  queue_narrow f ~applied:false [];
  queue_narrow f ~applied:false [];
  let o = ok (Session.next_number s "12345") in
  check_bool "dead end" true o.dead_end;
  check_int "unchanged" 4 o.main;
  check_ids "empty groups are not compared" [ "i32" ] (last_narrow_ids f);
  (* the fallback compares with 60, the last number that applied *)
  queue_narrow f ~applied:false [];
  queue_narrow f ~applied:true [ gi "i32" 2 ];
  let o = ok (Session.next_number s "70") in
  check_bool "went up" true (List.mem (Session.Fell_back Up) o.notes);
  check_pred "compared with 60" Gt_prev (last_narrow_pred f "i32");
  check_int "main" 2 o.main;
  queue_narrow f ~applied:false [];
  queue_narrow f ~applied:true [ gi "i32" 1 ];
  let o = ok (Session.next_number s "70") in
  check_bool "stayed the same" true (List.mem (Session.Fell_back Same) o.notes);
  check_pred "same" Eq_prev (last_narrow_pred f "i32")

let untracked_rescan () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 50; gi "f32" 20; gi ~tracked:false "u8" 4_000_000 ];
  let o = ok (Session.find s "30") in
  check_bool "untracked note" true (List.mem Session.Untracked o.notes);
  check_int "untracked not counted" 0 o.wide;
  check_int "main" 70 o.main;
  queue_narrow f ~applied:false [];
  queue_scan f [ gi "u8" 3 ];
  let o = ok (Session.next_number s "29") in
  check_bool "rescan keep" true (last_scan_keep f);
  check_int "wide" 3 o.wide;
  check_bool "showing wide" true (List.mem Session.Showing_wide o.notes);
  check_bool "old groups dropped" true (dropped f <> []);
  check_ids "untracked not narrowed" [ "i32"; "f32" ] (last_narrow_ids f);
  check_ids "rescanned" [ "u8" ] (last_scan_ids f);
  check_pred "rescan pred" (Eq 29L) (last_scan_spec f "u8").pred;
  check_ids "dropped" [ "i32"; "f32" ] (dropped f);
  check_int "main" 0 o.main;
  check_ids "groups" [ "u8" ] (ids_of o);
  check_bool "wide tier" true (List.map snd o.groups = [ Interp.Wide ]);
  check_bool "rechecked" false (List.mem Session.Untracked o.notes);
  check_bool "by tag" true (o.by_tag = [ ("whole number (1-byte)", 3) ])

let untracked_and_narrow_both_apply () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 50; gi "f32" 20; gi ~tracked:false "u8" 4_000_000 ];
  ignore (ok (Session.find s "30"));
  queue_narrow f ~applied:true [ gi "i32" 1; gi "f32" 0 ];
  queue_scan f [ gi "u8" 0 ];
  let o = ok (Session.next_number s "29") in
  check_int "main" 1 o.main;
  check_int "wide" 0 o.wide;
  check_ids "nothing dropped" [] (dropped f);
  check_ids "groups" [ "i32"; "f32"; "u8" ] (ids_of o);
  check_bool "u8 is tracked now" false (List.mem Session.Untracked o.notes)

let dead_end_keeps_untracked () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 50; gi ~tracked:false "u8" 4_000_000 ];
  ignore (ok (Session.find s "30"));
  queue_narrow f ~applied:false [];
  queue_scan f [ gi "u8" 0 ];
  queue_narrow f ~applied:false [];
  let o = ok (Session.next_number s "29") in
  check_bool "dead end" true o.dead_end;
  check_ids "nothing dropped" [] (dropped f);
  check_int "main" 50 o.main;
  check_bool "still untracked" true (List.mem Session.Untracked o.notes);
  (* rechecked on the next number *)
  queue_narrow f ~applied:true [ gi "i32" 5 ];
  queue_scan f [ gi "u8" 2 ];
  let o = ok (Session.next_number s "28") in
  check_ids "rescanned again" [ "u8" ] (last_scan_ids f);
  check_int "main" 5 o.main;
  check_int "wide" 2 o.wide

let unfit_groups () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 500; gi "f32" 40; gi "u8" 30 ];
  ignore (ok (Session.find s "100"));
  queue_narrow f ~applied:false [];
  queue_narrow f ~applied:false [];
  let o = ok (Session.next_number s "300") in
  check_bool "dead end" true o.dead_end;
  check_ids "u8 cannot hold 300" [ "i32"; "f32" ] (last_narrow_ids f);
  check_ids "a dead end drops nothing" [] (dropped f);
  check_ids "groups kept" [ "i32"; "f32"; "u8" ] (ids_of o);
  queue_narrow f ~applied:true [ gi "i32" 2; gi "f32" 0 ];
  let o = ok (Session.next_number s "300") in
  check_ids "narrowed" [ "i32"; "f32" ] (last_narrow_ids f);
  check_ids "dropped once applied" [ "u8" ] (dropped f);
  check_ids "groups" [ "i32"; "f32" ] (ids_of o)

let relations () =
  let s, f = fresh () in
  queue_scan f [ gi ~values:false "i32" 3_000_000; gi ~values:false "f32" 2_500_000 ];
  ignore (ok (Session.find s "1"));
  check_err "too many" "too many matches to compare by change yet. type the new number instead." (Session.next_relation s Down);
  check_int "nothing sent" 0 (narrow_calls f);
  let s, f = fresh () in
  queue_scan f [ gi "i32" 500; gi ~values:false "f32" 3_000_000; gi ~tracked:false "u8" 9_000_000 ];
  ignore (ok (Session.find s "100"));
  queue_narrow f ~applied:true [ gi "i32" 7 ];
  let o = ok (Session.next_relation s Changed) in
  check_ids "only groups with values" [ "i32" ] (last_narrow_ids f);
  check_pred "changed" Ne_prev (last_narrow_pred f "i32");
  check_bool "atomic" true (last_narrow_atomic f);
  check_int "others left alone" 3_000_007 o.main;
  check_int "no rescan" 1 (scan_calls f);
  check_bool "no fallback" false (fell_back o);
  queue_narrow f ~applied:false [];
  let o = ok (Session.next_relation s Down) in
  check_bool "dead end" true o.dead_end;
  check_int "unchanged" 3_000_007 o.main;
  check_pred "down" Lt_prev (last_narrow_pred f "i32");
  queue_narrow f ~applied:true [ gi "i32" 7 ];
  ignore (ok (Session.next_relation s Up));
  check_pred "up" Gt_prev (last_narrow_pred f "i32");
  queue_narrow f ~applied:true [ gi "i32" 7 ];
  ignore (ok (Session.next_relation s Same));
  check_pred "same" Eq_prev (last_narrow_pred f "i32");
  (* an empty hunt has nothing left to compare *)
  let s, f = fresh () in
  queue_scan f [ gi "i32" 0 ];
  ignore (ok (Session.find s "100"));
  check_bool "empty hunt" true (ok (Session.next_relation s Down)).dead_end;
  check_int "nothing sent" 0 (narrow_calls f)

let results_set_undo () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 5000; gi "f32" 300 ];
  ignore (ok (Session.find s "100"));
  queue_narrow f ~applied:true [ gi "i32" 1; gi "f32" 0 ];
  ignore (ok (Session.next_number s "85"));
  queue_peek f "i32" [ it 0x10008L (Vint 85L) ];
  let rows = ok (Session.results s ~all:false) in
  check_str "row" "85" (List.hd rows).shown;
  check_str "tag" "whole number" (List.hd rows).tag;
  check_bool "row" true (rows = [ { n = 1; addr = 0x10008L; interp_id = "i32"; tag = "whole number"; shown = "85" } ]);
  check_bool "one page of 10" true (peeks f = [ ("i32", 0, 10) ]);
  queue_write f ~prev:(Vint 85L);
  check_str "set" "set #1 to 999 (was 85)" (ok (Session.set s ~row:1 "999"));
  check_bool "wrote" true (last_write f = (0x10008L, "i32", Vint 999L));
  check_bool "recorded" true (Session.writes s = [ (0x10008L, "i32", Vint 85L, Vint 999L) ]);
  queue_write f ~prev:(Vint 999L);
  check_str "undo" "restored #1 to 85" (ok (Session.undo s));
  check_bool "wrote back" true (last_write f = (0x10008L, "i32", Vint 85L));
  check_bool "undone" true (Session.writes s = []);
  check_err "undo twice" "nothing to undo" (Session.undo s);
  check_err "bad row" "no result #9. run: moth results" (Session.set s ~row:9 "1");
  check_err "not a number" "'x' isn't a number" (Session.set s ~row:1 "x");
  let s_u8, f_u8 = fresh () in
  queue_scan f_u8 [ gi "i32" 0; gi "u8" 1 ];
  ignore (ok (Session.find s_u8 "100"));
  queue_peek f_u8 "u8" [ it 0x20000L (Vint 100L) ];
  check_str "wide row" "whole number (1-byte)" (List.hd (ok (Session.results s_u8 ~all:false))).tag;
  check_err "does not fit" "999999 doesn't fit in a whole number (1-byte)" (Session.set s_u8 ~row:1 "999999");
  check_int "nothing written" 0 (write_calls f_u8)

let undo_order () =
  let s, f = fresh () in
  queue_scan f [ gi "i32x100" 1 ];
  ignore (ok (Session.find s "1"));
  queue_peek f "i32x100" [ it 0x5000L (Vint 100L) ];
  ignore (ok (Session.results s ~all:false));
  queue_write f ~prev:(Vint 100L);
  check_str "first" "set #1 to 2.50 (was 1.00)" (ok (Session.set s ~row:1 "2.5"));
  queue_write f ~prev:(Vint 250L);
  check_str "second" "set #1 to 9.00 (was 2.50)" (ok (Session.set s ~row:1 "9"));
  check_bool "newest first" true (Session.writes s = [ (0x5000L, "i32", Vint 250L, Vint 900L); (0x5000L, "i32", Vint 100L, Vint 250L) ]);
  queue_write f ~prev:(Vint 900L);
  check_str "undo newest" "restored #1 to 2.50" (ok (Session.undo s));
  queue_write f ~prev:(Vint 250L);
  check_str "undo oldest" "restored #1 to 1.00" (ok (Session.undo s));
  check_bool "wrote the first value back" true (last_write f = (0x5000L, "i32", Vint 100L))

let listing_follows_hunt () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 2 ];
  ignore (ok (Session.find s "100"));
  queue_peek f "i32" [ it 0x1000L (Vint 100L); it 0x2000L (Vint 100L) ];
  ignore (ok (Session.results s ~all:false));
  queue_narrow f ~applied:false [];
  queue_narrow f ~applied:false [];
  check_bool "dead end" true (ok (Session.next_number s "5")).dead_end;
  queue_write f ~prev:(Vint 100L);
  check_str "a dead end keeps the listing" "set #2 to 7 (was 100)" (ok (Session.set s ~row:2 "7"));
  queue_narrow f ~applied:true [ gi "i32" 1 ];
  ignore (ok (Session.next_number s "100"));
  check_err "a step replaces the listing" "no result #2. run: moth results" (Session.set s ~row:2 "7")

let results_selection () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 2; gi "f32" 2000; gi "i16" 0; gi "u8" 1; gi "i32x100" 1; gi ~tracked:false "i32u" 50 ];
  ignore (ok (Session.find s "100"));
  queue_peek f "i32" [ it 0x1000L (Vint 100L); it 0x2000L (Vint 100L) ];
  let rows = ok (Session.results s ~all:false) in
  check_bool "main only, up to 1000 matches" true (peeks f = [ ("i32", 0, 10) ]);
  check_bool "numbered" true (List.map (fun (r : Session.row) -> r.n) rows = [ 1; 2 ]);
  queue_peek f "i32" [ it 0x1000L (Vint 100L); it 0x2000L (Vint 100L) ];
  queue_peek f "u8" [ it 0x3001L (Vint 100L) ];
  queue_peek f "i32x100" [ it 0x4000L (Vint 10000L) ];
  let rows = ok (Session.results s ~all:true) in
  check_bool "all, in plan order" true (List.tl (peeks f) = [ ("i32", 0, 10); ("u8", 0, 10); ("i32x100", 0, 10) ]);
  check_bool "numbered across groups" true (List.map (fun (r : Session.row) -> r.n) rows = [ 1; 2; 3; 4 ]);
  check_ids "interpretations" [ "i32"; "i32"; "u8"; "i32x100" ] (List.map (fun (r : Session.row) -> r.interp_id) rows);
  let last = List.nth rows 3 in
  check_str "scaled shown" "100.00" last.shown;
  check_str "scaled tag" "whole number ×100" last.tag;
  check_bool "address" true (last.addr = 0x4000L)

let stats_and_lost () =
  let s, f = fresh () in
  queue_scan ~stats:(stats ~bytes:4096 2.5) f [ gi "i32" 50; gi ~tracked:false "u8" 4_000_000 ];
  let o = ok (Session.find s "30") in
  check_int "bytes" 4096 o.bytes;
  check_bool "ms" true (o.ms = 2.5);
  queue_narrow ~stats:(stats ~bytes:12 ~lost:2 1.) f ~applied:true [ gi "i32" 3 ];
  queue_scan ~stats:(stats ~bytes:1000 3.) f [ gi "u8" 0 ];
  let o = ok (Session.next_number s "29") in
  check_bool "lost" true (List.mem (Session.Lost 2) o.notes);
  check_int "bytes summed" 1012 o.bytes;
  check_bool "ms summed" true (o.ms = 4.)

let out_of_order () =
  let fresh_s, _ = fresh () in
  check_err "no hunt" no_hunt (Session.next_number fresh_s "5");
  check_err "no hunt relation" no_hunt (Session.next_relation fresh_s Down);
  check_err "no hunt results" no_hunt (Session.results fresh_s ~all:false);
  check_err "no hunt set" no_hunt (Session.set fresh_s ~row:1 "5");
  check_err "nothing to undo" "nothing to undo" (Session.undo fresh_s);
  check_err "bad number" "'abc' isn't a number" (Session.find fresh_s "abc")

let game_closes () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 500 ];
  ignore (ok (Session.find s "100"));
  queue_error f "narrow" gone;
  check_err "closed" closed (Session.next_number s "5");
  check_err "cleared" no_hunt (Session.next_number s "5");
  (* a write made in that game cannot be undone in the next one *)
  let s, f = fresh () in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "100"));
  queue_peek f "i32" [ it 0x10008L (Vint 100L) ];
  ignore (ok (Session.results s ~all:false));
  queue_write f ~prev:(Vint 100L);
  ignore (ok (Session.set s ~row:1 "5"));
  queue_error f "peek" gone;
  check_err "closed during results" closed (Session.results s ~all:false);
  check_err "undo cleared" "nothing to undo" (Session.undo s);
  check_bool "writes cleared" true (Session.writes s = []);
  (* other helper errors pass through and keep the hunt *)
  let s, f = fresh () in
  queue_scan f [ gi "i32" 500 ];
  ignore (ok (Session.find s "100"));
  queue_error f "narrow" { code = "no_such_group"; message = "no group i32" };
  check_err "passed through" "no group i32" (Session.next_number s "5");
  queue_narrow f ~applied:true [ gi "i32" 1 ];
  check_int "hunt kept" 1 (ok (Session.next_number s "5")).main

let () =
  let t n f = Alcotest.test_case n `Quick f in
  Alcotest.run "session"
    [
      ( "hunt",
        [
          t "find sends the plan" find_sends_plan;
          t "next narrows" next_narrows;
          t "fallback and dead end" fallback_and_dead_end;
          t "untracked rescan" untracked_rescan;
          t "untracked and narrow both apply" untracked_and_narrow_both_apply;
          t "dead end keeps untracked" dead_end_keeps_untracked;
          t "unfit groups" unfit_groups;
          t "relations" relations;
          t "stats and lost" stats_and_lost;
        ] );
      ( "results",
        [
          t "results, set, undo" results_set_undo;
          t "undo order" undo_order;
          t "listing follows the hunt" listing_follows_hunt;
          t "results selection" results_selection;
        ] );
      ("errors", [ t "out of order" out_of_order; t "game closes" game_closes ]);
    ]
