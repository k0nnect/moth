open Moth_core
open Helper_api
open Fake_helper

let ok = function Ok x -> x | Error e -> Alcotest.failf "unexpected error: %s" e
let said r = fst (ok r)
let check_int n w g = Alcotest.(check int) n w g
let check_bool n w g = Alcotest.(check bool) n w g
let check_str n w g = Alcotest.(check string) n w g
let check_ids n w g = Alcotest.(check (list string)) n w g
let check_held n w g = Alcotest.(check (option bool)) n w g

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
let it addr v : item = { addr; v = Some v }
let hole addr : item = { addr; v = None }

let fresh () =
  let h, f = Fake_helper.create () in
  (Session.create ~settle:(fun () -> ()) h, f)

let set_text r = (ok r : Session.set_done).text

(* the first page, as the daemon lists it by default *)
let first_page ?(all = false) s = Session.results s ~all ~from:1 ~limit:20 ~near:None
let rows_of r = (ok r : Session.listing).rows

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
  (* a fallback is a relation step, so no number is the baseline after it: no exact match is a
     dead end, which leaves the hunt as it was *)
  queue_narrow f ~applied:false [];
  let o = ok (Session.next_number s "12345") in
  check_bool "dead end" true o.dead_end;
  check_int "unchanged" 4 o.main;
  check_int "no fallback after a fallback" 3 (narrow_calls f);
  (* a number that applies is the baseline again; a dead end leaves it as it was *)
  queue_narrow f ~applied:true [ gi "i32" 3; gi "f32" 0 ];
  ignore (ok (Session.next_number s "60"));
  queue_narrow f ~applied:false [];
  queue_narrow f ~applied:false [];
  let o = ok (Session.next_number s "12345") in
  check_bool "dead end" true o.dead_end;
  check_int "unchanged" 3 o.main;
  check_ids "empty groups are not compared" [ "i32" ] (last_narrow_ids f);
  (* the fallback compares with 60, the last number that applied *)
  queue_narrow f ~applied:false [];
  queue_narrow f ~applied:true [ gi "i32" 2 ];
  let o = ok (Session.next_number s "70") in
  check_bool "went up" true (List.mem (Session.Fell_back Up) o.notes);
  check_pred "compared with 60" Gt_prev (last_narrow_pred f "i32");
  check_int "main" 2 o.main;
  queue_narrow f ~applied:true [ gi "i32" 2 ];
  ignore (ok (Session.next_number s "70"));
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
  let rows = rows_of (first_page s) in
  check_str "row" "85" (List.hd rows).shown;
  check_str "tag" "whole number" (List.hd rows).tag;
  check_bool "row" true (rows = [ { n = 1; addr = 0x10008L; interp_id = "i32"; tag = "whole number"; shown = "85"; away = None } ]);
  check_bool "only the rows the page needs" true (peeks f = [ ("i32", 0, 1) ]);
  queue_write f ~prev:(Vint 85L) ~prev_bits:0x55L;
  queue_read f (Vint 999L);
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
  check_str "wide row" "whole number (1-byte)" (List.hd (rows_of (first_page s_u8))).tag;
  check_err "does not fit" "999999 doesn't fit in a whole number (1-byte)" (Session.set s_u8 ~row:1 "999999");
  check_int "nothing written" 0 (write_calls f_u8)

let undo_order () =
  let s, f = fresh () in
  queue_scan f [ gi "i32x100" 1 ];
  ignore (ok (Session.find s "1"));
  queue_peek f "i32x100" [ it 0x5000L (Vint 100L) ];
  ignore (ok (first_page s));
  queue_write f ~prev:(Vint 100L) ~prev_bits:100L;
  queue_read f (Vint 250L);
  check_str "first" "set #1 to 2.50 (was 1.00)" (set_text (Session.set s ~row:1 "2.5"));
  queue_write f ~prev:(Vint 250L) ~prev_bits:250L;
  queue_read f (Vint 900L);
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
  check_str "a nan is shown" "nan" (List.hd (rows_of (first_page s))).shown;
  check_bool "peeked as its type" true (peek_vtypes f = [ ("f32", "f32") ]);
  queue_write f ~prev:(Vfloat Float.nan) ~prev_bits:0x7fc00001L;
  queue_read f (Vfloat 5.);
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
  ignore (ok (first_page s));
  queue_narrow f ~applied:false [];
  queue_narrow f ~applied:false [];
  check_bool "dead end" true (ok (Session.next_number s "5")).dead_end;
  queue_write f ~prev:(Vint 100L);
  queue_read f (Vint 7L);
  check_str "a dead end keeps the listing" "set #2 to 7 (was 100)" (set_text (Session.set s ~row:2 "7"));
  queue_narrow f ~applied:true [ gi "i32" 1 ];
  ignore (ok (Session.next_number s "100"));
  check_err "a step replaces the listing" "no result #2. run: moth results" (Session.set s ~row:2 "7")

let results_selection () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 2; gi "f32" 2000; gi "i16" 0; gi "u8" 1; gi "i32x100" 1; gi ~tracked:false "i32u" 50 ];
  ignore (ok (Session.find s "100"));
  queue_peek f "i32" [ it 0x1000L (Vint 100L); it 0x2000L (Vint 100L) ];
  queue_peek f "f32" (List.init 18 (fun k -> it (Int64.of_int (0x8000 + (4 * k))) (Vfloat 100.)));
  let rows = rows_of (first_page s) in
  check_bool "main only, however many matches" true (peeks f = [ ("i32", 0, 2); ("f32", 0, 18) ]);
  check_bool "numbered" true (List.map (fun (r : Session.row) -> r.n) rows = List.init 20 (fun k -> k + 1));
  queue_peek f "u8" [ it 0x3001L (Vint 100L) ];
  queue_peek f "i32x100" [ it 0x4000L (Vint 10000L) ];
  queue_peek f "i32" [ it 0x1000L (Vint 100L); it 0x2000L (Vint 100L) ];
  let rows = rows_of (Session.results s ~all:true ~from:1 ~limit:4 ~near:None) in
  check_bool "all, fewest first, ties in plan order" true
    (List.filteri (fun k _ -> k >= 2) (peeks f) = [ ("u8", 0, 1); ("i32x100", 0, 1); ("i32", 0, 2) ]);
  check_bool "numbered across groups" true (List.map (fun (r : Session.row) -> r.n) rows = [ 1; 2; 3; 4 ]);
  check_ids "interpretations" [ "u8"; "i32x100"; "i32"; "i32" ] (List.map (fun (r : Session.row) -> r.interp_id) rows);
  let scaled = List.nth rows 1 in
  check_str "scaled shown" "100.00" scaled.shown;
  check_str "scaled tag" "whole number ×100" scaled.tag;
  check_bool "address" true (scaled.addr = 0x4000L)

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
  (* a relation step that is a dead end changes nothing, so the baseline stays *)
  queue_narrow f ~applied:false [];
  check_bool "relation dead end" true (ok (Session.next_relation s Up)).dead_end;
  queue_narrow f ~applied:false [];
  queue_narrow f ~applied:true [ gi "i32" 2 ];
  let o = ok (Session.next_number s "85") in
  check_bool "fell back" true (List.mem (Session.Fell_back Down) o.notes);
  check_pred "compared with 90" Lt_prev (last_narrow_pred f "i32")

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
  ignore (ok (first_page s));
  queue_write f ~prev:(Vint 100L) ~prev_bits:100L;
  queue_read f (Vint 5L);
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
  check_err "lost listing results" hunt_lost (first_page s);
  check_err "cleared too" no_hunt (first_page s);
  let s, f = fresh () in
  queue_scan f [ gi "i32" 9 ];
  ignore (ok (Session.find s "100"));
  queue_error f "narrow" { code = "no_such_group"; message = "no group \"i32\"" };
  check_err "lost in a relation step" hunt_lost (Session.next_relation s Down)

let out_of_order () =
  let fresh_s, _ = fresh () in
  check_err "no hunt" no_hunt (Session.next_number fresh_s "5");
  check_err "no hunt relation" no_hunt (Session.next_relation fresh_s Down);
  check_err "no hunt results" no_hunt (first_page fresh_s);
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
  ignore (ok (first_page s));
  queue_write f ~prev:(Vint 100L);
  queue_read f (Vint 5L);
  ignore (set_text (Session.set s ~row:1 "5"));
  queue_error f "peek" gone;
  check_err "closed during results" closed (first_page s);
  check_err "undo cleared" "nothing to undo" (Session.undo s);
  check_bool "writes cleared" true (Session.writes s = []);
  (* losing the helper ends the hunt the same way *)
  let s, f = fresh () in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "100"));
  queue_peek f "i32" [ it 0x10008L (Vint 100L) ];
  ignore (ok (first_page s));
  queue_write f ~prev:(Vint 100L);
  queue_read f (Vint 5L);
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
  ignore (ok (first_page s));
  queue_write f ~prev:(Vfloat 6.) ~prev_bits:0x40c00000L;
  queue_read f (Vfloat 9.);
  ignore (ok (Session.set s ~row:1 "9"));
  queue_narrow f ~applied:true [ gi "f32" 1 ];
  ignore (ok (Session.next_number s "7"));
  check_pred "after a set" (Range { lo = 6.; hi = 8.; floor = 1e-6; moved = false }) (last_narrow_pred f "f32")

(* R12: a fallback is a relation step. The stored values hold what it read, not the number typed,
   so the right number after a mistyped one is not asked to have moved *)
let past_a_fallback () =
  let s, f = fresh () in
  queue_scan f [ gi "f32" 1 ];
  ignore (ok (Session.find s "5"));
  queue_narrow f ~applied:false [];
  queue_narrow f ~applied:true [ gi "f32" 1 ];
  check_bool "fell back up" true (List.mem (Session.Fell_back Up) (ok (Session.next_number s "7")).notes);
  queue_narrow f ~applied:true [ gi "f32" 1 ];
  let o = ok (Session.next_number s "6") in
  check_pred "the right number, not asked to have moved" (Range { lo = 5.; hi = 7.; floor = 1e-6; moved = false }) (last_narrow_pred f "f32");
  check_int "it applies" 1 o.main;
  check_bool "no fallback" false (fell_back o);
  check_int "one narrow for it" 3 (narrow_calls f)

(* and with no baseline, a number with no exact match after a fallback is a dead end, not a
   second guess *)
let no_second_guess_after_a_fallback () =
  let s, f = fresh () in
  queue_scan f [ gi "f32" 1; gi "i32" 2 ];
  ignore (ok (Session.find s "5"));
  queue_narrow f ~applied:false [];
  queue_narrow f ~applied:true [ gi "f32" 1; gi "i32" 1 ];
  ignore (ok (Session.next_number s "7"));
  queue_narrow f ~applied:false [];
  let o = ok (Session.next_number s "9") in
  check_bool "dead end" true o.dead_end;
  check_int "exactly one narrow for it" 3 (narrow_calls f);
  check_pred "only the exact number" (Eq 9L) (last_narrow_pred f "i32");
  check_int "unchanged" 2 o.main

(* R13: moved is asked for only when the ranges the two numbers give a group overlap in part or
   not at all; a range that holds the other is a more (or less) exact number for the same value *)
let a_more_exact_number_is_not_a_move () =
  let moved f id = match last_narrow_pred f id with Range r -> r.moved | p -> Alcotest.failf "%s is not a range: %s" id (show_pred p) in
  let s, f = fresh () in
  queue_scan f [ gi "f32" 2; gi "f32pct" 2 ];
  ignore (ok (Session.find s "5"));
  queue_narrow f ~applied:true [ gi "f32" 1; gi "f32pct" 1 ];
  ignore (ok (Session.next_number s "5.3"));
  check_bool "5 then 5.3" false (moved f "f32");
  check_bool "5 then 5.3, the ÷100 reading" false (moved f "f32pct");
  queue_narrow f ~applied:true [ gi "f32" 1; gi "f32pct" 1 ];
  ignore (ok (Session.next_number s "5"));
  check_bool "5.3 then 5" false (moved f "f32");
  check_bool "5.3 then 5, the ÷100 reading" false (moved f "f32pct");
  queue_narrow f ~applied:true [ gi "f32" 1; gi "f32pct" 1 ];
  ignore (ok (Session.next_number s "6"));
  check_pred "5 then 6" (Range { lo = 5.; hi = 7.; floor = 1e-6; moved = true }) (last_narrow_pred f "f32");
  check_bool "5 then 6, the ÷100 reading" true (moved f "f32pct");
  (* a find for 5.3, then 5 *)
  let s, f = fresh () in
  queue_scan f [ gi "f32" 2 ];
  ignore (ok (Session.find s "5.3"));
  queue_narrow f ~applied:true [ gi "f32" 1 ];
  ignore (ok (Session.next_number s "5"));
  check_pred "found as 5.3, then 5" (Range { lo = 4.; hi = 6.; floor = 1e-6; moved = false }) (last_narrow_pred f "f32")

let a_write_the_game_puts_back () =
  let h, f = Fake_helper.create () in
  let s = Session.create ~settle:(fun () -> ()) h in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "6"));
  queue_peek f "i32" [ it 0x10L (Vint 6L) ];
  ignore (ok (first_page s));
  queue_write f ~prev:(Vint 6L) ~prev_bits:6L;
  queue_read f (Vint 6L);
  let d = ok (Session.set s ~row:1 "9") in
  check_held "held" (Some false) d.held;
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
  ignore (ok (first_page s));
  queue_write f ~prev:(Vfloat 100.) ~prev_bits:0x42c80000L;
  queue_read f (Vfloat (Int32.float_of_bits (Int32.bits_of_float 0.1)));
  let d = ok (Session.set s ~row:1 "0.1") in
  check_held "held" (Some true) d.held;
  check_str "the stage 1 line" "set #1 to 0.1 (was 100)" d.text;
  check_bool "read back where it wrote, as its type" true
    (List.exists (function Read { addr = 0x7000L; vtype = "f32" } -> true | _ -> false) (calls f))

let a_write_the_game_keeps_changing () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "6"));
  queue_peek f "i32" [ it 0x10L (Vint 6L) ];
  ignore (ok (first_page s));
  queue_write f ~prev:(Vint 6L) ~prev_bits:6L;
  queue_read f (Vint 7L);
  let d = ok (Session.set s ~row:1 "9") in
  check_held "held" (Some false) d.held;
  check_str "text" "set #1 to 9 (was 6)\nit already reads 7: the game keeps changing it." d.text;
  check_bool "still undoable" true (Session.writes s = [ (0x10L, "i32", Vint 6L, Vint 9L) ])

let a_write_that_cannot_be_read_back () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "6"));
  queue_peek f "i32" [ it 0x10L (Vint 6L) ];
  ignore (ok (first_page s));
  queue_write f ~prev:(Vint 6L) ~prev_bits:6L;
  queue_error f "read" { code = "read_failed"; message = "couldn't read 0x10" };
  let d = ok (Session.set s ~row:1 "9") in
  check_held "not known" None d.held;
  check_str "a second line says why" "set #1 to 9 (was 6)\nmoth couldn't read it back to check." d.text;
  check_bool "still undoable" true (Session.writes s = [ (0x10L, "i32", Vint 6L, Vint 9L) ])

(* R15: a read-back that could not happen is neither held nor not held, and a second line says
   why. Losing the game or the helper there ends the hunt, the kept values and the undo list, as
   any step's would; the write is reported either way. *)
let a_write_that_could_not_be_checked () =
  List.iter
    (fun ((e : err), why, ends) ->
      let n what = e.code ^ ": " ^ what in
      (* a row *)
      let s, f = fresh () in
      queue_read f (Vint 15L);
      ignore (ok (Session.keep_at s ~addr:0x6c551068L "15" ~name:(Some "xp")));
      queue_scan f [ gi "i32" 1 ];
      ignore (ok (Session.find s "6"));
      queue_peek f "i32" [ it 0x10L (Vint 6L) ];
      ignore (ok (first_page s));
      queue_write f ~prev:(Vint 6L) ~prev_bits:6L;
      queue_error f "read" e;
      let d = ok (Session.set s ~row:1 "9") in
      check_held (n "held") None d.held;
      check_str (n "text") ("set #1 to 9 (was 6)\n" ^ why) d.text;
      check_bool (n "the write") true (d.w = { where = 0x10L; wtype = "i32"; bits_before = 6L; bits_after = 9L });
      if ends then (
        check_bool (n "hunt cleared") true (Session.current s = None);
        check_bool (n "undo cleared") true (Session.writes s = []);
        check_bool (n "kept values cleared") true (ok (Session.kept s) = []))
      else (
        check_bool (n "hunt kept") true (Session.current s <> None);
        check_bool (n "still undoable") true (Session.writes s = [ (0x10L, "i32", Vint 6L, Vint 9L) ]);
        queue_read f (Vint 15L);
        check_ids (n "still kept") [ "xp" ] (List.map (fun (k : Session.kept) -> k.name) (ok (Session.kept s))));
      (* a kept value *)
      let s, f = fresh () in
      queue_read f (Vint 15L);
      ignore (ok (Session.keep_at s ~addr:0x6c551068L "15" ~name:(Some "xp")));
      queue_write f ~prev:(Vint 15L) ~prev_bits:15L;
      queue_error f "read" e;
      let d = ok (Session.set_kept s ~name:"xp" "95") in
      check_held (n "held, by name") None d.held;
      check_str (n "text, by name") ("set xp to 95 (was 15)\n" ^ why) d.text;
      check_bool (n "the write, by name") true (d.w = { where = 0x6c551068L; wtype = "i32"; bits_before = 15L; bits_after = 95L });
      if ends then (
        check_bool (n "undo cleared, by name") true (Session.writes s = []);
        check_bool (n "kept values cleared, by name") true (ok (Session.kept s) = []))
      else check_bool (n "still undoable, by name") true (Session.writes s = [ (0x6c551068L, "i32", Vint 15L, Vint 95L) ]))
    [
      (gone, "the game closed right after, so moth couldn't check it.", true);
      ({ code = "helper_lost"; message = "" }, "moth lost contact with its helper right after, so it couldn't check it.", true);
      ({ code = "read_failed"; message = "cannot read 0x10" }, "moth couldn't read it back to check.", false);
      ({ code = "bad_reply"; message = "x" }, "moth couldn't read it back to check.", false);
    ]

(* the stored values hold the written number now, not the last number typed *)
let a_set_or_undo_clears_the_baseline () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "61"));
  queue_peek f "i32" [ it 0x10L (Vint 61L) ];
  ignore (ok (first_page s));
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

(* the wait comes between the write and the read; a failed write waits for nothing. A write that
   landed is reported even when the game closes before the read-back, which ends the hunt, the
   kept values and the undo list there. *)
let a_write_check_when_things_fail () =
  let h, f = Fake_helper.create () in
  let settles = ref [] in
  let s = Session.create ~settle:(fun () -> settles := (match f.calls with Write _ :: _ -> "after the write" | _ -> "elsewhere") :: !settles) h in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "6"));
  queue_peek f "i32" [ it 0x10L (Vint 6L) ];
  ignore (ok (first_page s));
  queue_error f "write" { code = "write_failed"; message = "couldn't write 0x10" };
  check_err "the write failed" "couldn't write 0x10" (Session.set s ~row:1 "9");
  check_bool "no wait" true (!settles = []);
  check_bool "no read" false (List.exists (function Read _ -> true | _ -> false) (calls f));
  check_bool "nothing recorded" true (Session.writes s = []);
  queue_write f ~prev:(Vint 6L) ~prev_bits:6L;
  queue_error f "read" gone;
  let d = ok (Session.set s ~row:1 "9") in
  check_held "not known" None d.held;
  check_str "a second line says why" "set #1 to 9 (was 6)\nthe game closed right after, so moth couldn't check it." d.text;
  check_bool "the write is reported" true (d.w = { where = 0x10L; wtype = "i32"; bits_before = 6L; bits_after = 9L });
  check_bool "waited once, after the write" true (!settles = [ "after the write" ]);
  check_err "hunt cleared" no_hunt (Session.next_number s "5");
  check_err "undo cleared" "nothing to undo" (Session.undo s);
  check_bool "no write call since" true (write_calls f = 2)

(* ---- pages, nearness and kept values ---- *)

let near ?(from = 1) ?(limit = 20) s target = Session.results s ~all:false ~from ~limit ~near:(Some target)
let aways (l : Session.listing) = List.map (fun (r : Session.row) -> Option.get r.away) l.rows
let ns (l : Session.listing) = List.map (fun (r : Session.row) -> r.n) l.rows
let kept_one name kaddr ktag kshown : Session.kept = { name; kaddr; ktag; kshown }
let kept_names s = List.map (fun (k : Session.kept) -> k.name) (ok (Session.kept s))
let unknown_xp = "no kept value called xp. see: moth kept"
let name_rule = "a name is lowercase letters, digits, - and _, starting with a letter."

let small_groups_come_first_and_pages_continue () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 30; gi "f32" 0; gi "i64" 2 ];
  ignore (ok (Session.find s "5"));
  queue_peek f "i64" [ it 0x100L (Vint 5L); it 0x200L (Vint 5L) ];
  queue_peek f "i32" [ it 0x10L (Vint 5L); it 0x14L (Vint 5L); it 0x18L (Vint 5L) ];
  let l = ok (Session.results s ~all:false ~from:1 ~limit:5 ~near:None) in
  check_int "total" 32 l.total;
  check_ids "order" [ "i64"; "i64"; "i32"; "i32"; "i32" ] (List.map (fun (r : Session.row) -> r.interp_id) l.rows);
  check_bool "the i32 page asked for" true (last_peek f "i32" = (0, 3));
  queue_peek f "i32" [ it 0x1cL (Vint 5L) ];
  let l = ok (Session.results s ~all:false ~from:6 ~limit:1 ~near:None) in
  check_int "numbered on" 6 (List.hd l.rows).n;
  check_bool "offset 3" true (last_peek f "i32" = (3, 1));
  check_err "past the end" "no result #33. there are 32." (Session.results s ~all:false ~from:33 ~limit:5 ~near:None)

let pages_across_groups () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 2500; gi "f32" 1; gi "i16" 3 ];
  ignore (ok (Session.find s "5"));
  let i32s first k = List.init k (fun i -> it (Int64.of_int (0x10000 + (4 * (first + i)))) (Vint 5L)) in
  queue_peek f "f32" [ it 0x100L (Vfloat 5.) ];
  queue_peek f "i16" [ it 0x200L (Vint 5L); it 0x202L (Vint 5L); it 0x204L (Vint 5L) ];
  queue_peek f "i32" (i32s 0 1);
  let l = ok (Session.results s ~all:false ~from:1 ~limit:5 ~near:None) in
  check_ids "a page across three groups" [ "f32"; "i16"; "i16"; "i16"; "i32" ] (List.map (fun (r : Session.row) -> r.interp_id) l.rows);
  check_bool "each asked for its part" true (peeks f = [ ("f32", 0, 1); ("i16", 0, 3); ("i32", 0, 1) ]);
  (* more rows of one group than the helper gives in one peek *)
  queue_peek f "i32" (i32s 0 1000);
  queue_peek f "i32" (i32s 1000 1000);
  queue_peek f "i32" (i32s 2000 500);
  let l = ok (Session.results s ~all:false ~from:5 ~limit:3000 ~near:None) in
  check_bool "in peeks of at most 1000" true (List.filteri (fun k _ -> k >= 3) (peeks f) = [ ("i32", 0, 1000); ("i32", 1000, 1000); ("i32", 2000, 500) ]);
  check_int "to the last row" 2500 (List.length l.rows);
  check_int "numbered on" 2504 (List.nth l.rows 2499).n;
  check_int "from" 5 l.from;
  queue_peek f "i32" (i32s 2499 1);
  let l = ok (Session.results s ~all:false ~from:2504 ~limit:20 ~near:None) in
  check_bool "the last row alone" true (List.map (fun (r : Session.row) -> (r.n, r.addr)) l.rows = [ (2504, Int64.of_int (0x10000 + (4 * 2499))) ]);
  check_bool "asked for one" true (last_peek f "i32" = (2499, 1));
  (* nothing to list is a listing with no rows, wherever it starts *)
  let s, f = fresh () in
  queue_scan f [ gi "i32" 0 ];
  ignore (ok (Session.find s "5"));
  let l = ok (Session.results s ~all:false ~from:3 ~limit:20 ~near:None) in
  check_bool "empty" true (l.rows = [] && l.total = 0 && l.from = 3);
  check_bool "nothing asked" true (peeks f = [])

let near_a_kept_value () =
  let s, f = fresh () in
  queue_read f (Vint 15L);
  check_str "kept" "kept 0x6c551068 as xp (15, whole number)" (ok (Session.keep_at s ~addr:0x6c551068L "15" ~name:(Some "xp")));
  queue_scan f [ gi "i32x100" 5093; gi "i64x100" 304 ];
  ignore (ok (Session.find s "5.32"));
  queue_peek_from f "i64x100" [ it 0x6c551078L (Vint 532L); it 0x6c559000L (Vint 532L) ];
  queue_peek_from f "i32x100" [ it 0x6c551078L (Vint 532L); it 0x6c559000L (Vint 532L) ];
  let l = ok (Session.results s ~all:false ~from:1 ~limit:20 ~near:(Some "xp")) in
  check_int "two rows, both at the money address" 2 l.total;
  check_bool "away" true (List.for_all (fun (r : Session.row) -> r.addr = 0x6c551078L && r.away = Some 16) l.rows);
  check_bool "asked from reach below" true (last_peek_from_addr f "i32x100" = Int64.sub 0x6c551068L 4096L);
  check_ids "a tie goes in plan order" [ "i32x100"; "i64x100" ] (List.map (fun (r : Session.row) -> r.interp_id) l.rows);
  check_bool "numbered" true (ns l = [ 1; 2 ]);
  check_bool "64 at a time" true (List.for_all (fun (_, _, limit) -> limit = 64) (peek_froms f));
  check_str "shown" "5.32" (List.hd l.rows).shown

let near_an_address () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 3 ];
  ignore (ok (Session.find s "5"));
  queue_peek_from f "i32" [ it 0x6c551060L (Vint 5L); it 0x6c551068L (Vint 5L); it 0x6c551070L (Vint 5L) ];
  let l = ok (Session.results s ~all:false ~from:1 ~limit:20 ~near:(Some "0x6c551068")) in
  check_bool "nothing kept" true (ok (Session.kept s) = []);
  check_bool "its own address first, then below before above" true (aways l = [ 0; -8; 8 ] && ns l = [ 1; 2; 3 ]);
  queue_write f ~prev:(Vint 5L) ~prev_bits:5L;
  queue_read f (Vint 9L);
  check_str "set takes the near listing" "set #1 to 9 (was 5)" (set_text (Session.set s ~row:1 "9"));
  check_bool "at the address" true (last_write f = (0x6c551068L, "i32", Put_value (Vint 9L)))

let near_nothing () =
  let s, f = fresh () in
  check_err "no hunt" no_hunt (near s "0x10");
  queue_scan f [ gi "i32" 3 ];
  ignore (ok (Session.find s "5"));
  check_err "an unknown name" unknown_xp (near s "xp");
  check_bool "nothing asked" true (peek_froms f = []);
  queue_read f (Vint 5L);
  ignore (ok (Session.keep_at s ~addr:0x6c551068L "5" ~name:(Some "xp")));
  queue_peek_from f "i32" [ it 0x6c553000L (Vint 5L) ];
  check_err "nothing in reach" "nothing within 4 kb of xp." (near s "xp")

(* 64 at a time from reach below, on from the last one, until one is more than reach above *)
let near_pages_and_the_ends_of_memory () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 5000 ];
  ignore (ok (Session.find s "5"));
  let below = List.init 64 (fun k -> it (Int64.of_int (4 * k)) (Vint 5L)) in
  let above = [ it 0x100L (Vint 5L); it 0x104L (Vint 5L); it 0x1100L (Vint 5L); it 0x1104L (Vint 5L) ] in
  queue_peek_from f "i32" below;
  queue_peek_from f "i32" above;
  let l = ok (near ~limit:100 s "0x100") in
  check_bool "from the start of memory, then on" true (peek_froms f = [ ("i32", 0L, 64); ("i32", 0xfdL, 64) ]);
  check_int "every one within reach" 67 l.total;
  check_bool "nearest first" true (List.filteri (fun k _ -> k < 4) (aways l) = [ 0; -4; 4; -8 ]);
  check_bool "reach is inclusive" true (List.nth (aways l) 65 = -256 && List.nth (aways l) 66 = 4096);
  queue_peek_from f "i32" below;
  queue_peek_from f "i32" above;
  let l = ok (near ~from:66 ~limit:5 s "0x100") in
  check_bool "paged" true (ns l = [ 66; 67 ] && aways l = [ -256; 4096 ] && l.total = 67 && l.from = 66);
  queue_peek_from f "i32" below;
  queue_peek_from f "i32" above;
  check_err "past the end" "no result #68. there are 67." (near ~from:68 s "0x100");
  queue_peek_from f "i32" [ it 0xfffffffffffff010L (Vint 5L); it 0xfffffffffffffff0L (Vint 5L) ];
  let l = ok (near s "0xFFFFFFFFFFFFF000") in
  check_bool "at the top of memory, from reach below" true (last_peek_from_addr f "i32" = 0xffffffffffffe000L);
  check_bool "nothing wraps around" true (aways l = [ 16; 4080 ])

(* R14: a position whose value can't be read is a row in its place, shown as ?, so rows are
   numbered by position; it is set and kept as any row is *)
let rows_that_cannot_be_read () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 6 ];
  ignore (ok (Session.find s "77"));
  let at k = Int64.of_int (0x20ff0 + (4 * (k - 1))) in
  (* rows 3 and 4 are on a page the game freed after the step *)
  queue_peek f "i32" [ hole (at 3); hole (at 4); it (at 5) (Vint 77L); it (at 6) (Vint 77L) ];
  let l = ok (Session.results s ~all:false ~from:3 ~limit:4 ~near:None) in
  check_bool "asked from row 3" true (last_peek f "i32" = (2, 4));
  check_bool "numbered by position, ? where it can't be read" true
    (List.map (fun (r : Session.row) -> (r.n, r.addr, r.shown)) l.rows = [ (3, at 3, "?"); (4, at 4, "?"); (5, at 5, "77"); (6, at 6, "77") ]);
  queue_peek f "i32" [ it (at 5) (Vint 77L); it (at 6) (Vint 77L) ];
  let l = ok (Session.results s ~all:false ~from:5 ~limit:4 ~near:None) in
  check_bool "the same positions, the same numbers" true (List.map (fun (r : Session.row) -> (r.n, r.addr)) l.rows = [ (5, at 5); (6, at 6) ]);
  queue_peek f "i32" [ it (at 1) (Vint 77L); it (at 2) (Vint 77L); hole (at 3); hole (at 4) ];
  ignore (ok (Session.results s ~all:false ~from:1 ~limit:4 ~near:None));
  check_str "kept as any row" "kept #3 as v1 (?, whole number)" (ok (Session.keep_row s ~row:3 ~name:None));
  queue_error f "write" { code = "write_failed"; message = "cannot write 0x20ff8" };
  check_err "set as any row: the write says why it failed" "cannot write 0x20ff8" (Session.set s ~row:3 "9");
  check_bool "written where the row is" true (last_write f = (at 3, "i32", Put_value (Vint 9L)));
  (* the first rows can't be read: they are still rows *)
  let s, f = fresh () in
  queue_scan f [ gi "i32" 15360 ];
  ignore (ok (Session.find s "0"));
  queue_peek f "i32" [ hole 0x20000L; hole 0x20004L; hole 0x20008L ];
  let l = ok (Session.results s ~all:false ~from:1 ~limit:3 ~near:None) in
  check_bool "three rows of ?" true (List.map (fun (r : Session.row) -> (r.n, r.shown)) l.rows = [ (1, "?"); (2, "?"); (3, "?") ] && l.total = 15360)

(* the page below the value was freed after the step: its positions come back as holes, and the
   listing near the value pages on past them *)
let near_past_a_freed_page () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 15360 ];
  ignore (ok (Session.find s "0"));
  queue_peek_from f "i32" (List.init 64 (fun k -> hole (Int64.of_int (0x20010 + (4 * k)))));
  queue_peek_from f "i32" [ it 0x21010L (Vint 0L); it 0x21014L (Vint 0L); it 0x22100L (Vint 0L) ];
  let l = ok (near ~limit:3 s "0x21010") in
  check_bool "paged on past the holes" true (peek_froms f = [ ("i32", 0x20010L, 64); ("i32", 0x2010dL, 64) ]);
  check_int "the holes in reach are rows too" 66 l.total;
  check_bool "the readable rows around them, nearest first" true
    (List.map (fun (r : Session.row) -> (r.n, r.addr, r.shown, r.away)) l.rows
    = [ (1, 0x21010L, "0", Some 0); (2, 0x21014L, "0", Some 4); (3, 0x2010cL, "?", Some (-3844)) ])

(* R7: set and keep take the rows of the latest listing only *)
let only_the_latest_listing () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 8 ];
  ignore (ok (Session.find s "5"));
  let at k = Int64.of_int (0x1000 + (4 * k)) in
  queue_peek f "i32" (List.init 5 (fun k -> it (at k) (Vint 5L)));
  ignore (ok (Session.results s ~all:false ~from:1 ~limit:5 ~near:None));
  queue_peek f "i32" (List.init 3 (fun k -> it (at (5 + k)) (Vint 5L)));
  ignore (ok (Session.results s ~all:false ~from:6 ~limit:5 ~near:None));
  check_err "a row of the page before" "no result #1. run: moth results" (Session.set s ~row:1 "9");
  check_err "nor kept" "no result #1. run: moth results" (Session.keep_row s ~row:1 ~name:None);
  check_int "nothing written" 0 (write_calls f);
  queue_write f ~prev:(Vint 5L) ~prev_bits:5L;
  queue_read f (Vint 9L);
  check_str "a row of the latest" "set #6 to 9 (was 5)" (set_text (Session.set s ~row:6 "9"));
  check_bool "at its position" true (last_write f = (at 5, "i32", Put_value (Vint 9L)))

let a_kept_row_outlives_the_hunt () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "15"));
  queue_peek f "i32" [ it 0x6c551068L (Vint 15L) ];
  ignore (ok (first_page s));
  check_str "kept" "kept #1 as xp (15, whole number)" (ok (Session.keep_row s ~row:1 ~name:(Some "xp")));
  check_err "a row not listed" "no result #2. run: moth results" (Session.keep_row s ~row:2 ~name:(Some "hp"));
  queue_scan f [ gi "i32" 40 ];
  ignore (ok (Session.find s "20"));
  queue_read f (Vint 20L);
  check_bool "kept through a new find, read now" true (ok (Session.kept s) = [ kept_one "xp" 0x6c551068L "whole number" "20" ]);
  check_bool "read as its type" true (reads f = [ (0x6c551068L, "i32") ]);
  check_err "a row needs a listing" "no result #1. run: moth results" (Session.keep_row s ~row:1 ~name:(Some "hp"))

let keep_at_an_address () =
  let s, f = fresh () in
  let plan = Interp.plan (Value.Int 15L) in
  List.iter (fun (i : Interp.t) -> queue_read f (if i.vtype = "f32" || i.vtype = "f64" then Vfloat 16. else Vint 16L)) plan;
  check_err "nothing there" "nothing at 0x6c551068 reads as 15." (Session.keep_at s ~addr:0x6c551068L "15" ~name:(Some "xp"));
  check_bool "each interpretation in plan order, as its type" true (reads f = List.map (fun (i : Interp.t) -> (0x6c551068L, i.vtype)) plan);
  check_bool "nothing kept" true (ok (Session.kept s) = []);
  (* the first that holds wins; one that can't be read there is passed over *)
  queue_read f (Vint 0x41700000L);
  queue_read f (Vfloat 15.);
  check_str "a decimal" "kept 0x20 as v1 (15, decimal)" (ok (Session.keep_at s ~addr:0x20L "15" ~name:None));
  queue_error f "read" { code = "read_failed"; message = "cannot read 0x30" };
  queue_read f (Vfloat 15.);
  check_str "past a read that failed" "kept 0x30 as v2 (15, decimal)" (ok (Session.keep_at s ~addr:0x30L "15" ~name:None));
  check_err "not a number" "'x' isn't a number" (Session.keep_at s ~addr:0x40L "x" ~name:None)

let a_kept_value_that_cannot_be_read () =
  let s, f = fresh () in
  queue_read f (Vint 15L);
  ignore (ok (Session.keep_at s ~addr:0x6c551068L "15" ~name:(Some "xp")));
  queue_read f (Vint 16L);
  ignore (ok (Session.keep_at s ~addr:0x6c551070L "16" ~name:(Some "hp")));
  queue_error f "read" { code = "read_failed"; message = "cannot read 0x6c551068" };
  queue_read f (Vint 17L);
  check_bool "shown as ?, the rest read, as kept" true
    (ok (Session.kept s) = [ kept_one "xp" 0x6c551068L "whole number" "?"; kept_one "hp" 0x6c551070L "whole number" "17" ])

let names () =
  let s, f = fresh () in
  List.iter (fun n -> check_err n name_rule (Session.keep_at s ~addr:0x10L "15" ~name:(Some n))) [ "Xp"; "1up"; String.make 25 'a'; ""; "x p"; "0x10" ];
  check_bool "nothing read" true (reads f = []);
  queue_read f (Vint 15L);
  check_str "xp" "kept 0x10 as xp (15, whole number)" (ok (Session.keep_at s ~addr:0x10L "15" ~name:(Some "xp")));
  check_err "taken" "there's already a kept value called xp. pick another, or: moth forget xp" (Session.keep_at s ~addr:0x20L "15" ~name:(Some "xp"));
  queue_read f (Vint 15L);
  check_str "the first free name" "kept 0x30 as v1 (15, whole number)" (ok (Session.keep_at s ~addr:0x30L "15" ~name:None));
  queue_read f (Vint 15L);
  check_str "the next" "kept 0x40 as v2 (15, whole number)" (ok (Session.keep_at s ~addr:0x40L "15" ~name:None));
  let long = "a-b_c9" ^ String.make 18 'z' in
  queue_read f (Vint 15L);
  check_str "24, with digits, - and _" ("kept 0x50 as " ^ long ^ " (15, whole number)") (ok (Session.keep_at s ~addr:0x50L "15" ~name:(Some long)));
  (* a row's name is held to the same rule *)
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "15"));
  queue_peek f "i32" [ it 0x60L (Vint 15L) ];
  ignore (ok (first_page s));
  check_err "a number is not a name" name_rule (Session.keep_row s ~row:1 ~name:(Some "5"));
  check_err "nor is an address" name_rule (Session.keep_row s ~row:1 ~name:(Some "0x10"));
  List.iter (fun _ -> queue_read f (Vint 15L)) [ 1; 2; 3; 4 ];
  check_ids "nothing else kept" [ "xp"; "v1"; "v2"; long ] (kept_names s)

let at_most_64_kept () =
  let s, f = fresh () in
  for k = 1 to 64 do
    queue_read f (Vint 15L);
    ignore (ok (Session.keep_at s ~addr:(Int64.of_int (16 * k)) "15" ~name:None))
  done;
  let full = "that's 64 kept values, the most moth holds. forget one first." in
  check_err "full" full (Session.keep_at s ~addr:0x10000L "15" ~name:(Some "xp"));
  check_err "full, with no name" full (Session.keep_at s ~addr:0x10000L "15" ~name:None);
  check_str "forget" "forgot v7" (ok (Session.forget s "v7"));
  check_err "forgotten" "no kept value called v7. see: moth kept" (Session.forget s "v7");
  queue_read f (Vint 15L);
  check_str "its name is free again" "kept 0x10000 as v7 (15, whole number)" (ok (Session.keep_at s ~addr:0x10000L "15" ~name:None));
  for _ = 1 to 64 do queue_read f (Vint 15L) done;
  let names = kept_names s in
  check_int "64" 64 (List.length names);
  check_str "kept last, listed last" "v7" (List.nth names 63)

let set_a_kept_value () =
  let s, f = fresh () in
  queue_read f (Vint 15L);
  ignore (ok (Session.keep_at s ~addr:0x6c551068L "15" ~name:(Some "xp")));
  queue_write f ~prev:(Vint 15L) ~prev_bits:15L;
  queue_read f (Vint 95L);
  let d = ok (Session.set_kept s ~name:"xp" "95") in
  check_str "set" "set xp to 95 (was 15)" d.text;
  check_held "held" (Some true) d.held;
  check_bool "the write" true (d.w = { where = 0x6c551068L; wtype = "i32"; bits_before = 15L; bits_after = 95L });
  check_bool "wrote as its type" true (last_write f = (0x6c551068L, "i32", Put_value (Vint 95L)));
  check_bool "recorded" true (Session.writes s = [ (0x6c551068L, "i32", Vint 15L, Vint 95L) ]);
  queue_write f ~prev:(Vint 95L) ~prev_bits:95L;
  check_str "undo" "restored xp to 15" (said (Session.undo s));
  check_bool "wrote back the bytes" true (last_write f = (0x6c551068L, "i32", Put_bits 15L));
  queue_write f ~prev:(Vint 15L) ~prev_bits:15L;
  queue_read f (Vint 15L);
  check_str "checked as set is"
    "set xp to 95 (was 15)\nthe game put it straight back to 15. it's worked out from something else, so writing here won't stick."
    (set_text (Session.set_kept s ~name:"xp" "95"));
  check_err "a number it can't hold" "this value is a whole number, and 1.5 has a decimal part" (Session.set_kept s ~name:"xp" "1.5");
  check_str "forgot" "forgot xp" (ok (Session.forget s "xp"));
  queue_write f ~prev:(Vint 15L) ~prev_bits:15L;
  check_str "a forgotten value's write is still undone by name" "restored xp to 15" (said (Session.undo s));
  (* it clears the baseline as set does *)
  let s, f = fresh () in
  queue_scan f [ gi "i32" 3 ];
  ignore (ok (Session.find s "61"));
  queue_read f (Vint 61L);
  ignore (ok (Session.keep_at s ~addr:0x10L "61" ~name:(Some "hp")));
  queue_write f ~prev:(Vint 61L) ~prev_bits:61L;
  queue_read f (Vint 999L);
  ignore (ok (Session.set_kept s ~name:"hp" "999"));
  queue_narrow f ~applied:false [];
  check_bool "dead end" true (ok (Session.next_number s "995")).dead_end;
  check_int "no fallback narrow" 1 (narrow_calls f)

let unknown_names () =
  let s, f = fresh () in
  check_err "set" unknown_xp (Session.set_kept s ~name:"xp" "95");
  check_err "forget" unknown_xp (Session.forget s "xp");
  (* a name with the form of an address says how to change one *)
  check_err "set, an address"
    "no kept value called 0x6c551068. to change an address, keep it first: moth keep 0x6c551068 <the number it shows>"
    (Session.set_kept s ~name:"0x6c551068" "95");
  check_err "set, an address in capitals"
    "no kept value called 0X6C551068. to change an address, keep it first: moth keep 0X6C551068 <the number it shows>"
    (Session.set_kept s ~name:"0X6C551068" "95");
  check_err "set, not quite an address" "no kept value called 0x6g. see: moth kept" (Session.set_kept s ~name:"0x6g" "95");
  check_err "forget, an address" "no kept value called 0x6c551068. see: moth kept" (Session.forget s "0x6c551068");
  check_int "nothing written" 0 (write_calls f)

let kept_values_and_a_closed_game () =
  let s, f = fresh () in
  queue_read f (Vint 15L);
  ignore (ok (Session.keep_at s ~addr:0x6c551068L "15" ~name:(Some "xp")));
  (* a lost group ends the hunt, not the kept values *)
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "5"));
  queue_error f "peek" { code = "no_such_group"; message = "no group \"i32\"" };
  check_err "hunt lost" hunt_lost (first_page s);
  queue_read f (Vint 15L);
  check_bool "still kept" true (ok (Session.kept s) = [ kept_one "xp" 0x6c551068L "whole number" "15" ]);
  queue_error f "read" gone;
  check_err "closed" closed (Session.kept s);
  check_bool "cleared" true (ok (Session.kept s) = []);
  check_err "set by name" unknown_xp (Session.set_kept s ~name:"xp" "9")

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
          t "past a fallback" past_a_fallback;
          t "no second guess after a fallback" no_second_guess_after_a_fallback;
          t "a more exact number is not a move" a_more_exact_number_is_not_a_move;
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
          t "a write that could not be checked" a_write_that_could_not_be_checked;
        ] );
      ("errors", [ t "out of order" out_of_order; t "game closes" game_closes; t "failed find, lost groups" failed_find_and_lost_groups ]);
      ( "pages, near, kept",
        [
          t "small groups come first and pages continue" small_groups_come_first_and_pages_continue;
          t "pages across groups" pages_across_groups;
          t "near a kept value" near_a_kept_value;
          t "near an address" near_an_address;
          t "near nothing, or no such name" near_nothing;
          t "near: pages and the ends of memory" near_pages_and_the_ends_of_memory;
          t "rows that cannot be read" rows_that_cannot_be_read;
          t "near, past a freed page" near_past_a_freed_page;
          t "only the latest listing" only_the_latest_listing;
          t "a kept row outlives the hunt" a_kept_row_outlives_the_hunt;
          t "keep at an address" keep_at_an_address;
          t "a kept value that cannot be read" a_kept_value_that_cannot_be_read;
          t "names" names;
          t "at most 64 kept" at_most_64_kept;
          t "set a kept value" set_a_kept_value;
          t "unknown names" unknown_names;
          t "kept values and a closed game" kept_values_and_a_closed_game;
        ] );
    ]
