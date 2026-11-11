open Moth_core
open Helper_api

let dec s = match Value.parse s with Ok v -> v | Error e -> failwith e
let check_ids n got want = Alcotest.(check (list string)) n want got
let check_tier n (w : Interp.tier) g = Alcotest.(check bool) n true (w = g)
let check_int n w g = Alcotest.(check int) n w g
let check_str n w g = Alcotest.(check string) n w g
let feq a b = Float.abs (a -. b) < 1e-9
let check_pred n (w : Helper_api.pred option) (g : Helper_api.pred option) =
  let same =
    match (w, g) with
    | Some (Range (a, b)), Some (Range (c, d)) -> feq a c && feq b d
    | a, b -> a = b
  in
  Alcotest.(check bool) n true same
let check_enc n (w : (Helper_api.value, string) result) g =
  let same =
    match (w, g) with
    | Ok (Vfloat a), Ok (Vfloat b) -> feq a b
    | a, b -> a = b
  in
  Alcotest.(check bool) n true same
let check_err n msg g = Alcotest.(check bool) n true (g = Error msg)

let ids v = List.map (fun (i : Interp.t) -> i.id) (Interp.plan v)

let cases () =
  let open Value in
  check_ids "100" (ids (Int 100L)) [ "i32"; "f32"; "i16"; "i64"; "f64"; "u8"; "i32x100"; "i32x10"; "i32x1000"; "f32pct"; "i32u"; "f32u" ];
  check_ids "70000" (ids (Int 70000L)) [ "i32"; "f32"; "i64"; "f64"; "i32x100"; "i32x10"; "i32x1000"; "f32pct"; "i32u"; "f32u" ];
  check_ids "40000" (ids (Int 40000L)) [ "i32"; "f32"; "u16"; "i64"; "f64"; "i32x100"; "i32x10"; "i32x1000"; "f32pct"; "i32u"; "f32u" ];
  check_ids "3e9" (ids (Int 3_000_000_000L)) [ "u32"; "f32"; "i64"; "f64"; "f32pct"; "u32u"; "f32u" ];
  check_ids "-3" (ids (Int (-3L))) [ "i32"; "f32"; "i16"; "i64"; "f64"; "i8"; "i32x100"; "i32x10"; "i32x1000"; "f32pct"; "i32u"; "f32u" ];
  check_ids "132.50" (ids (dec "132.50")) [ "f32"; "f64"; "i32x100"; "i64x100"; "f32u" ];
  let i id v = List.find (fun (x : Interp.t) -> x.id = id) (Interp.plan v) in
  check_tier "u8 is wide" Wide (i "u8" (Int 100L)).tier;
  check_tier "i32x100 of a decimal is main" Main (i "i32x100" (dec "132.50")).tier;
  check_int "i32u excludes aligned" 4 (i "i32u" (Int 100L)).exclude_align;
  check_pred "eq" (Some (Eq 100L)) (Interp.pred_for (i "i32" (Int 100L)) (Int 100L));
  check_pred "float range" (Some (Range (99., 101.))) (Interp.pred_for (i "f32" (Int 100L)) (Int 100L));
  check_pred "scaled" (Some (Eq 8500L)) (Interp.pred_for (i "i32x100" (Int 100L)) (Int 85L));
  check_pred "pct" (Some (Range (0.84, 0.86))) (Interp.pred_for (i "f32pct" (Int 100L)) (Int 85L));
  check_pred "cents" (Some (Eq 13250L)) (Interp.pred_for (i "i32x100" (dec "132.50")) (dec "132.50"));
  check_pred "decimal range" (Some (Range (132.49, 132.51))) (Interp.pred_for (i "f32" (dec "132.50")) (dec "132.50"));
  check_pred "whole given decimal" None (Interp.pred_for (i "i32" (Int 100L)) (dec "85.5"));
  check_pred "does not fit" None (Interp.pred_for (i "u8" (Int 100L)) (Int 300L));
  check_enc "int" (Ok (Vint 999L)) (Interp.encode (i "i32" (Int 100L)) (Int 999L));
  check_enc "float" (Ok (Vfloat 999.)) (Interp.encode (i "f32" (Int 100L)) (Int 999L));
  check_enc "cents" (Ok (Vint 99999L)) (Interp.encode (i "i32x100" (dec "132.50")) (dec "999.99"));
  check_enc "pct" (Ok (Vfloat 0.5)) (Interp.encode (i "f32pct" (Int 100L)) (Int 50L));
  check_err "too big" "999999 doesn't fit in a whole number (1-byte)" (Interp.encode (i "u8" (Int 100L)) (Int 999999L));
  check_err "fraction" "this value is a whole number, and 1.5 has a decimal part" (Interp.encode (i "i32" (Int 100L)) (dec "1.5"));
  check_str "decode cents" "132.50" (Interp.decode (i "i32x100" (dec "132.50")) (Vint 13250L));
  check_str "decode float" "85.3" (Interp.decode (i "f32" (Int 100L)) (Vfloat 85.3));
  check_str "describe" "whole number ×100" (Interp.describe (i "i32x100" (Int 100L)));
  check_str "describe pct" "decimal ÷100" (Interp.describe (i "f32pct" (Int 100L)));
  check_str "describe unaligned" "whole number (unaligned)" (Interp.describe (i "i32u" (Int 100L)));
  check_str "decode negative cents" "-0.05" (Interp.decode (i "i32x100" (dec "132.50")) (Vint (-5L)))

let () = Alcotest.run "interp" [ ("plan", [ Alcotest.test_case "cases" `Quick cases ]) ]
