open Moth_core
open Helper_api
open Fake_helper

let ok = function Ok x -> x | Error e -> Alcotest.failf "unexpected error: %s" e
let said r = fst (ok r)
let check_int n w g = Alcotest.(check int) n w g
let check_bool n w g = Alcotest.(check bool) n w g
let check_str n w g = Alcotest.(check string) n w g
let check_ids n w g = Alcotest.(check (list string)) n w g

let show_pred = function
  | Eq n -> Printf.sprintf "eq %Ld" n
  | Range r -> Printf.sprintf "range %h %h %h %b" r.lo r.hi r.floor r.moved
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
  (Session.create ~settle:(fun () -> ()) h, f)

let set_text r = (ok r : Session.set_done).text

let stats ?(bytes = 0) ?(lost = 0) ?(unreadable = 0) ms = { zero_stats with bytes; lost; unreadable_bytes = unreadable; ms }
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
  check_pred "f32" (Range { lo = 84.; hi = 86.; floor = 1e-6; moved = true }) (last_narrow_pred f "f32");
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

let untracked_rescan_is_not_a_match () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 50; gi ~tracked:false "u8" 4_000_000 ];
  ignore (ok (Session.find s "30"));
  queue_narrow f ~applied:false [];
  queue_scan f [ gi ~tracked:false "u8" 3_900_000 ];
  queue_narrow f ~applied:true [ gi "i32" 4 ];
  let o = ok (Session.next_number s "29") in
  check_ids "nothing dropped" [] (dropped f);
  check_int "fallback narrow sent" 2 (narrow_calls f);
  check_pred "fallback" Lt_prev (last_narrow_pred f "i32");
  check_bool "fell back" true (List.mem (Session.Fell_back Down) o.notes);
  check_int "main" 4 o.main;
  check_bool "still untracked" true (List.mem Session.Untracked o.notes)

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
  queue_write f ~prev:(Vint 85L) ~prev_bits:0x55L;
  check_str "set" "set #1 to 999 (was 85)" (set_text (Session.set s ~row:1 "999"));
  check_bool "wrote" true (last_write f = (0x10008L, "i32", Put_value (Vint 999L)));
  check_bool "recorded" true (Session.writes s = [ (0x10008L, "i32", Vint 85L, Vint 999L) ]);
  queue_write f ~prev:(Vint 999L);
  check_str "undo" "restored #1 to 85" (said (Session.undo s));
  check_bool "wrote back the bytes" true (last_write f = (0x10008L, "i32", Put_bits 0x55L));
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
  queue_write f ~prev:(Vint 100L) ~prev_bits:100L;
  check_str "first" "set #1 to 2.50 (was 1.00)" (set_text (Session.set s ~row:1 "2.5"));
  queue_write f ~prev:(Vint 250L) ~prev_bits:250L;
  check_str "second" "set #1 to 9.00 (was 2.50)" (set_text (Session.set s ~row:1 "9"));
  check_bool "newest first" true (Session.writes s = [ (0x5000L, "i32", Vint 250L, Vint 900L); (0x5000L, "i32", Vint 100L, Vint 250L) ]);
  queue_write f ~prev:(Vint 900L);
  check_str "undo newest" "restored #1 to 2.50" (said (Session.undo s));
  queue_write f ~prev:(Vint 250L);
  check_str "undo oldest" "restored #1 to 1.00" (said (Session.undo s));
  check_bool "wrote the first value back" true (last_write f = (0x5000L, "i32", Put_bits 100L))

let undo_puts_bytes_back () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 0; gi "f32" 1 ];
  ignore (ok (Session.find s "100"));
  queue_peek f "f32" [ it 0x7000L (Vfloat Float.nan) ];
  check_str "a nan is shown" "nan" (List.hd (ok (Session.results s ~all:false))).shown;
  check_bool "peeked as its type" true (peek_vtypes f = [ ("f32", "f32") ]);
  queue_write f ~prev:(Vfloat Float.nan) ~prev_bits:0x7fc00001L;
  let d = ok (Session.set s ~row:1 "5") in
  check_str "set" "set #1 to 5 (was nan)" d.text;
  check_bool "the write" true (d.w = { where = 0x7000L; wtype = "f32"; bits_before = 0x7fc00001L; bits_after = 0x40a00000L });
  check_bool "wrote the value" true (last_write f = (0x7000L, "f32", Put_value (Vfloat 5.)));
  queue_write f ~prev:(Vfloat 5.) ~prev_bits:0x40a00000L;
  let text, w = ok (Session.undo s) in
  check_str "undo" "restored #1 to nan" text;
  check_bool "the undo's write" true (w = { where = 0x7000L; wtype = "f32"; bits_before = 0x40a00000L; bits_after = 0x7fc00001L });
  check_bool "wrote the bytes back" true (last_write f = (0x7000L, "f32", Put_bits 0x7fc00001L))

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
  check_str "a dead end keeps the listing" "set #2 to 7 (was 100)" (set_text (Session.set s ~row:2 "7"));
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
  queue_scan ~stats:(stats ~bytes:4096 ~unreadable:8192 2.5) f [ gi "i32" 50; gi ~tracked:false "u8" 4_000_000 ];
  let o = ok (Session.find s "30") in
  check_int "bytes" 4096 o.bytes;
  check_int "unreadable" 8192 o.unreadable;
  check_bool "ms" true (o.ms = 2.5);
  queue_narrow ~stats:(stats ~bytes:12 ~lost:2 1.) f ~applied:true [ gi "i32" 3 ];
  queue_scan ~stats:(stats ~bytes:1000 ~unreadable:4096 3.) f [ gi "u8" 0 ];
  let o = ok (Session.next_number s "29") in
  check_bool "lost" true (List.mem (Session.Lost 2) o.notes);
  check_int "bytes summed" 1012 o.bytes;
  check_int "unreadable of the rescan" 4096 o.unreadable;
  check_bool "ms summed" true (o.ms = 4.);
  queue_narrow f ~applied:true [ gi "i32" 1 ];
  queue_scan f [ gi "u8" 0 ];
  check_int "none this step" 0 (ok (Session.next_number s "28")).unreadable;
  check_int "nothing read for the hunt as it stands" 0 (Option.get (Session.current s)).unreadable

(* a relation step moves the helper's stored values on, so the last number typed no longer says
   which way a later number went *)
let relation_clears_the_baseline () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 500 ];
  ignore (ok (Session.find s "100"));
  queue_narrow f ~applied:true [ gi "i32" 40 ];
  ignore (ok (Session.next_relation s Down));
  queue_narrow f ~applied:false [];
  let o = ok (Session.next_number s "91") in
  check_bool "dead end" true o.dead_end;
  check_int "no fallback sent" 2 (narrow_calls f);
  check_pred "only the exact number" (Eq 91L) (last_narrow_pred f "i32");
  check_int "unchanged" 40 o.main;
  (* a number that applies is the baseline again *)
  queue_narrow f ~applied:true [ gi "i32" 5 ];
  ignore (ok (Session.next_number s "90"));
  queue_narrow f ~applied:false [];
  queue_narrow f ~applied:true [ gi "i32" 2 ];
  let o = ok (Session.next_number s "85") in
  check_bool "fell back" true (List.mem (Session.Fell_back Down) o.notes);
  check_pred "compared with 90" Lt_prev (last_narrow_pred f "i32");
  (* a relation step that is a dead end changes nothing, so the baseline stays *)
  queue_narrow f ~applied:false [];
  check_bool "relation dead end" true (ok (Session.next_relation s Up)).dead_end;
  queue_narrow f ~applied:false [];
  queue_narrow f ~applied:true [ gi "i32" 1 ];
  let o = ok (Session.next_number s "99") in
  check_bool "fell back up from 85" true (List.mem (Session.Fell_back Up) o.notes)

let hunt_lost = "the hunt was lost. start again with: moth find <number>"

let failed_find_and_lost_groups () =
  (* a find whose scan fails leaves no hunt behind *)
  let s, f = fresh () in
  queue_scan f [ gi "i32" 500 ];
  ignore (ok (Session.find s "100"));
  queue_error f "scan" { code = "bad_request"; message = "out of memory" };
  check_err "scan failed" "out of memory" (Session.find s "85");
  check_err "no hunt left" no_hunt (Session.next_number s "5");
  check_bool "nothing to show" true (Session.current s = None);
  (* the helper no longer has a group the hunt relies on: the hunt is lost, the writes are not *)
  let s, f = fresh () in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "100"));
  queue_peek f "i32" [ it 0x10008L (Vint 100L) ];
  ignore (ok (Session.results s ~all:false));
  queue_write f ~prev:(Vint 100L) ~prev_bits:100L;
  ignore (set_text (Session.set s ~row:1 "5"));
  queue_error f "narrow" { code = "no_such_group"; message = "no group \"i32\"" };
  check_err "lost in a step" hunt_lost (Session.next_number s "5");
  check_err "cleared" no_hunt (Session.next_number s "5");
  check_err "listing cleared" no_hunt (Session.set s ~row:1 "7");
  queue_write f ~prev:(Vint 5L) ~prev_bits:5L;
  check_str "the write can still be undone" "restored #1 to 100" (said (Session.undo s));
  let s, f = fresh () in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "100"));
  queue_error f "peek" { code = "no_such_group"; message = "no group \"i32\"" };
  check_err "lost listing results" hunt_lost (Session.results s ~all:false);
  check_err "cleared too" no_hunt (Session.results s ~all:false);
  let s, f = fresh () in
  queue_scan f [ gi "i32" 9 ];
  ignore (ok (Session.find s "100"));
  queue_error f "narrow" { code = "no_such_group"; message = "no group \"i32\"" };
  check_err "lost in a relation step" hunt_lost (Session.next_relation s Down)

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
  ignore (set_text (Session.set s ~row:1 "5"));
  queue_error f "peek" gone;
  check_err "closed during results" closed (Session.results s ~all:false);
  check_err "undo cleared" "nothing to undo" (Session.undo s);
  check_bool "writes cleared" true (Session.writes s = []);
  (* losing the helper ends the hunt the same way *)
  let s, f = fresh () in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "100"));
  queue_peek f "i32" [ it 0x10008L (Vint 100L) ];
  ignore (ok (Session.results s ~all:false));
  queue_write f ~prev:(Vint 100L);
  ignore (set_text (Session.set s ~row:1 "5"));
  queue_error f "narrow" { code = "helper_lost"; message = "" };
  check_err "helper lost" "lost contact with moth's helper. attach again." (Session.next_number s "5");
  check_err "hunt cleared" no_hunt (Session.next_number s "5");
  check_err "undo cleared too" "nothing to undo" (Session.undo s);
  (* other helper errors pass through and keep the hunt *)
  let s, f = fresh () in
  queue_scan f [ gi "i32" 500 ];
  ignore (ok (Session.find s "100"));
  queue_error f "narrow" { code = "bad_request"; message = "group listed twice: i32" };
  check_err "passed through" "group listed twice: i32" (Session.next_number s "5");
  queue_narrow f ~applied:true [ gi "i32" 1 ];
  check_int "hunt kept" 1 (ok (Session.next_number s "5")).main

let current () =
  let s, f = fresh () in
  check_bool "no hunt yet" true (Session.current s = None);
  queue_scan f ~stats:(stats ~bytes:4096 3.) [ gi "i32" 2; gi "f32" 1; gi "u8" 40; gi ~tracked:false "i32u" 900000 ];
  let found = ok (Session.find s "100") in
  let o = Option.get (Session.current s) in
  check_int "main" 3 o.main;
  check_int "wide" 40 o.wide;
  check_ids "groups" (ids_of found) (ids_of o);
  check_bool "by tag" true (o.by_tag = found.by_tag);
  check_bool "notes from the groups" true (o.notes = [ Session.Untracked ]);
  check_bool "not a dead end" false o.dead_end;
  check_bool "no step cost" true (o.ms = 0. && o.bytes = 0);
  queue_narrow f ~applied:true [ gi "i32" 1; gi "f32" 0; gi "u8" 0 ];
  queue_scan f [ gi ~tracked:false "i32u" 800000 ];
  ignore (ok (Session.next_number s "85"));
  check_int "follows the hunt" 1 (Option.get (Session.current s)).main;
  queue_error f "narrow" gone;
  ignore (Session.next_number s "5");
  check_bool "gone with the game" true (Session.current s = None)

(* a decimal between the two numbers typed must have moved to stay *)
let new_number_asks_floats_to_have_moved () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 3; gi "f32" 2; gi ~values:false "f64" 9 ];
  ignore (ok (Session.find s "5"));
  queue_narrow f ~applied:true [ gi "i32" 1; gi "f32" 1; gi ~values:false "f64" 9 ];
  ignore (ok (Session.next_number s "6"));
  check_pred "f32" (Range { lo = 5.; hi = 7.; floor = 1e-6; moved = true }) (last_narrow_pred f "f32");
  check_pred "f64 has no values" (Range { lo = 5.; hi = 7.; floor = 1e-6; moved = false }) (last_narrow_pred f "f64");
  check_pred "i32" (Eq 6L) (last_narrow_pred f "i32");
  queue_narrow f ~applied:true [ gi "i32" 1; gi "f32" 1 ];
  ignore (ok (Session.next_number s "6"));
  check_pred "the same number again" (Range { lo = 5.; hi = 7.; floor = 1e-6; moved = false }) (last_narrow_pred f "f32")

(* with no baseline, nothing says the value had to move *)
let no_baseline_nothing_moved () =
  let s, f = fresh () in
  queue_scan f [ gi "f32" 2 ];
  ignore (ok (Session.find s "5"));
  queue_narrow f ~applied:true [ gi "f32" 1 ];
  ignore (ok (Session.next_relation s Up));
  queue_narrow f ~applied:true [ gi "f32" 1 ];
  ignore (ok (Session.next_number s "6"));
  check_pred "after a relation step" (Range { lo = 5.; hi = 7.; floor = 1e-6; moved = false }) (last_narrow_pred f "f32");
  queue_peek f "f32" [ it 0x20L (Vfloat 6.) ];
  ignore (ok (Session.results s ~all:false));
  queue_write f ~prev:(Vfloat 6.) ~prev_bits:0x40c00000L;
  queue_read f (Vfloat 9.);
  ignore (ok (Session.set s ~row:1 "9"));
  queue_narrow f ~applied:true [ gi "f32" 1 ];
  ignore (ok (Session.next_number s "7"));
  check_pred "after a set" (Range { lo = 6.; hi = 8.; floor = 1e-6; moved = false }) (last_narrow_pred f "f32")

let a_write_the_game_puts_back () =
  let h, f = Fake_helper.create () in
  let s = Session.create ~settle:(fun () -> ()) h in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "6"));
  queue_peek f "i32" [ it 0x10L (Vint 6L) ];
  ignore (ok (Session.results s ~all:false));
  queue_write f ~prev:(Vint 6L) ~prev_bits:6L;
  queue_read f (Vint 6L);
  let d = ok (Session.set s ~row:1 "9") in
  check_bool "held" false d.held;
  check_str "text"
    "set #1 to 9 (was 6)\nthe game put it straight back to 6. it's worked out from something else, so writing here won't stick."
    d.text;
  check_bool "still undoable" true (Session.writes s = [ (0x10L, "i32", Vint 6L, Vint 9L) ])

(* an f32 reads back rounded to single precision; the bytes are what was written *)
let a_write_that_holds () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 0; gi "f32" 1 ];
  ignore (ok (Session.find s "100"));
  queue_peek f "f32" [ it 0x7000L (Vfloat 100.) ];
  ignore (ok (Session.results s ~all:false));
  queue_write f ~prev:(Vfloat 100.) ~prev_bits:0x42c80000L;
  queue_read f (Vfloat (Int32.float_of_bits (Int32.bits_of_float 0.1)));
  let d = ok (Session.set s ~row:1 "0.1") in
  check_bool "held" true d.held;
  check_str "the stage 1 line" "set #1 to 0.1 (was 100)" d.text;
  check_bool "read back where it wrote, as its type" true
    (List.exists (function Read { addr = 0x7000L; vtype = "f32" } -> true | _ -> false) (calls f))

let a_write_the_game_keeps_changing () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "6"));
  queue_peek f "i32" [ it 0x10L (Vint 6L) ];
  ignore (ok (Session.results s ~all:false));
  queue_write f ~prev:(Vint 6L) ~prev_bits:6L;
  queue_read f (Vint 7L);
  let d = ok (Session.set s ~row:1 "9") in
  check_bool "held" false d.held;
  check_str "text" "set #1 to 9 (was 6)\nit already reads 7: the game keeps changing it." d.text;
  check_bool "still undoable" true (Session.writes s = [ (0x10L, "i32", Vint 6L, Vint 9L) ])

let a_write_that_cannot_be_read_back () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "6"));
  queue_peek f "i32" [ it 0x10L (Vint 6L) ];
  ignore (ok (Session.results s ~all:false));
  queue_write f ~prev:(Vint 6L) ~prev_bits:6L;
  queue_error f "read" { code = "read_failed"; message = "couldn't read 0x10" };
  let d = ok (Session.set s ~row:1 "9") in
  check_bool "held" true d.held;
  check_str "one line" "set #1 to 9 (was 6)" d.text;
  check_bool "still undoable" true (Session.writes s = [ (0x10L, "i32", Vint 6L, Vint 9L) ])

(* the stored values hold the written number now, not the last number typed *)
let a_set_or_undo_clears_the_baseline () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "61"));
  queue_peek f "i32" [ it 0x10L (Vint 61L) ];
  ignore (ok (Session.results s ~all:false));
  queue_write f ~prev:(Vint 61L) ~prev_bits:61L;
  queue_read f (Vint 999L);
  ignore (ok (Session.set s ~row:1 "999"));
  queue_narrow f ~applied:false [];
  let o = ok (Session.next_number s "995") in
  check_bool "dead end" true o.dead_end;
  check_int "no fallback narrow" 1 (narrow_calls f);
  check_pred "only the exact number" (Eq 995L) (last_narrow_pred f "i32");
  queue_narrow f ~applied:true [ gi "i32" 1 ];
  ignore (ok (Session.next_number s "995"));
  queue_write f ~prev:(Vint 999L) ~prev_bits:999L;
  ignore (said (Session.undo s));
  queue_narrow f ~applied:false [];
  check_bool "dead end after an undo" true (ok (Session.next_number s "990")).dead_end;
  check_int "no fallback narrow after an undo" 3 (narrow_calls f)

let scans_carry_the_budget () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "30"));
  check_bool "none set" true (last_scan_budget f = None);
  let h, f = Fake_helper.create () in
  let s = Session.create ~budget_mb:64 ~settle:(fun () -> ()) h in
  queue_scan f [ gi "i32" 50; gi ~tracked:false "u8" 4_000_000 ];
  ignore (ok (Session.find s "30"));
  check_bool "find" true (last_scan_budget f = Some 64);
  queue_narrow f ~applied:true [ gi "i32" 5 ];
  queue_scan f [ gi "u8" 2 ];
  ignore (ok (Session.next_number s "29"));
  check_bool "a rescan" true (last_scan_keep f);
  check_bool "keeps no budget" true (last_scan_budget f = None)

(* the wait comes between the write and the read; a failed write waits for nothing *)
let a_write_check_when_things_fail () =
  let h, f = Fake_helper.create () in
  let settles = ref [] in
  let s = Session.create ~settle:(fun () -> settles := (match f.calls with Write _ :: _ -> "after the write" | _ -> "elsewhere") :: !settles) h in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "6"));
  queue_peek f "i32" [ it 0x10L (Vint 6L) ];
  ignore (ok (Session.results s ~all:false));
  queue_error f "write" { code = "write_failed"; message = "couldn't write 0x10" };
  check_err "the write failed" "couldn't write 0x10" (Session.set s ~row:1 "9");
  check_bool "no wait" true (!settles = []);
  check_bool "no read" false (List.exists (function Read _ -> true | _ -> false) (calls f));
  check_bool "nothing recorded" true (Session.writes s = []);
  queue_write f ~prev:(Vint 6L) ~prev_bits:6L;
  queue_error f "read" gone;
  check_err "closed during the check" closed (Session.set s ~row:1 "9");
  check_bool "waited once, after the write" true (!settles = [ "after the write" ]);
  check_err "hunt cleared" no_hunt (Session.next_number s "5");
  check_err "undo cleared" "nothing to undo" (Session.undo s)

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
          t "an untracked rescan is not a match" untracked_rescan_is_not_a_match;
          t "unfit groups" unfit_groups;
          t "relations" relations;
          t "stats and lost" stats_and_lost;
          t "a relation step clears the baseline" relation_clears_the_baseline;
          t "current" current;
          t "a new number asks floats to have moved" new_number_asks_floats_to_have_moved;
          t "no baseline, nothing moved" no_baseline_nothing_moved;
          t "a set or undo clears the baseline" a_set_or_undo_clears_the_baseline;
          t "scans carry the budget" scans_carry_the_budget;
        ] );
      ( "results",
        [
          t "results, set, undo" results_set_undo;
          t "undo order" undo_order;
          t "undo puts the bytes back" undo_puts_bytes_back;
          t "listing follows the hunt" listing_follows_hunt;
          t "results selection" results_selection;
          t "a write the game puts back" a_write_the_game_puts_back;
          t "a write that holds" a_write_that_holds;
          t "a write the game keeps changing" a_write_the_game_keeps_changing;
          t "a write that cannot be read back" a_write_that_cannot_be_read_back;
          t "a write check when things fail" a_write_check_when_things_fail;
        ] );
      ("errors", [ t "out of order" out_of_order; t "game closes" game_closes; t "failed find, lost groups" failed_find_and_lost_groups ]);
    ]
