type tier = Main | Wide
type encoding = Plain | Scaled of int | Fraction
type t = { id : string; vtype : string; align : int; exclude_align : int; tier : tier; encoding : encoding }

let is_float_type = function "f32" | "f64" -> true | _ -> false

let fits vtype n =
  match vtype with
  | "i8" -> n >= -128L && n <= 127L
  | "u8" -> n >= 0L && n <= 255L
  | "i16" -> n >= -32768L && n <= 32767L
  | "u16" -> n >= 0L && n <= 65535L
  | "i32" -> n >= -2147483648L && n <= 2147483647L
  | "u32" -> n >= 0L && n <= 4294967295L
  | "i64" -> true
  | "u64" -> n >= 0L
  | _ -> false

let pow10 p =
  let r = ref 1 in
  for _ = 1 to p do r := !r * 10 done;
  !r
let digits_of_scale k = String.length (string_of_int k) - 1

(* value * 10^p exactly: `Inexact when it has more decimal places than p that are not zeros,
   `Too_big when the product overflows int64 *)
let scale_value (v : Value.t) k =
  let p = digits_of_scale k in
  match v with
  | Value.Int n ->
      let kk = Int64.of_int k in
      if n > Int64.div Int64.max_int kk || n < Int64.div Int64.min_int kk then Error `Too_big else Ok (Int64.mul n kk)
  | Value.Dec { text; _ } -> (
      let neg = text.[0] = '-' in
      let body = if neg then String.sub text 1 (String.length text - 1) else text in
      match String.split_on_char '.' body with
      | [ ip; fp ] -> (
          let lf = String.length fp in
          if lf > p && String.exists (fun c -> c <> '0') (String.sub fp p (lf - p)) then Error `Inexact
          else
            let kept = if lf <= p then fp ^ String.make (p - lf) '0' else String.sub fp 0 p in
            match Int64.of_string_opt (ip ^ kept) with None -> Error `Too_big | Some n -> Ok (if neg then Int64.neg n else n))
      | _ -> Error `Too_big)

let text_of = function Value.Int n -> Int64.to_string n | Value.Dec d -> d.text

let eps_of = function Value.Int _ -> 1. | Value.Dec d -> 10. ** float_of_int (-d.decimals)

let mk id vtype align exclude_align tier encoding = { id; vtype; align; exclude_align; tier; encoding }

let plan (v : Value.t) =
  match v with
  | Value.Int n ->
      let some c x = if c then [ x ] else [] in
      let i32 = fits "i32" n and u32 = n > 2147483647L && fits "u32" n in
      let i32t = if i32 then "i32" else "u32" in
      let main =
        (if i32 then [ mk "i32" "i32" 4 0 Main Plain ] else some u32 (mk "u32" "u32" 4 0 Main Plain))
        @ [ mk "f32" "f32" 4 0 Main Plain ]
        @ (if fits "i16" n then [ mk "i16" "i16" 2 0 Main Plain ]
           else some (n > 32767L && n <= 65535L) (mk "u16" "u16" 2 0 Main Plain))
        @ [ mk "i64" "i64" 8 0 Main Plain; mk "f64" "f64" 8 0 Main Plain ]
      in
      let scaled k =
        match scale_value v k with
        | Ok p when fits "i32" p -> [ mk (Printf.sprintf "i32x%d" k) "i32" 4 0 Wide (Scaled k) ]
        | _ -> []
      in
      let wide =
        (if n < 0L then some (fits "i8" n) (mk "i8" "i8" 1 0 Wide Plain)
         else some (fits "u8" n) (mk "u8" "u8" 1 0 Wide Plain))
        @ scaled 100 @ scaled 10 @ scaled 1000
        @ [ mk "f32pct" "f32" 4 0 Wide Fraction ]
        @ (if i32 || u32 then [ mk (i32t ^ "u") i32t 1 4 Wide Plain ] else [])
        @ [ mk "f32u" "f32" 1 4 Wide Plain ]
      in
      main @ wide
  | Value.Dec d when d.decimals > 18 -> [ mk "f32" "f32" 4 0 Main Plain; mk "f64" "f64" 8 0 Main Plain; mk "f32u" "f32" 1 4 Wide Plain ]
  | Value.Dec d ->
      let k = pow10 d.decimals in
      let scaled vt =
        match scale_value v k with
        | Ok p when fits vt p -> [ mk (Printf.sprintf "%sx%d" vt k) vt (if vt = "i32" then 4 else 8) 0 Main (Scaled k) ]
        | _ -> []
      in
      [ mk "f32" "f32" 4 0 Main Plain; mk "f64" "f64" 8 0 Main Plain ]
      @ scaled "i32" @ scaled "i64"
      @ [ mk "f32u" "f32" 1 4 Wide Plain ]

let pred_for t (v : Value.t) : Helper_api.pred option =
  match t.encoding with
  | Plain when is_float_type t.vtype ->
      let c = Value.to_float v and e = eps_of v in
      Some (Range (c -. e, c +. e))
  | Plain -> (
      match v with
      | Value.Int n when fits t.vtype n -> Some (Eq n)
      | _ -> None)
  | Scaled k -> (
      match scale_value v k with
      | Ok p when fits t.vtype p -> Some (Eq p)
      | _ -> None)
  | Fraction ->
      let c = Value.to_float v and e = eps_of v in
      Some (Range ((c -. e) /. 100., (c +. e) /. 100.))

let describe t =
  match t.encoding with
  | Scaled k -> if t.vtype = "i64" then Printf.sprintf "whole number ×%d (8-byte)" k else Printf.sprintf "whole number ×%d" k
  | Fraction -> "decimal ÷100"
  | Plain ->
      if t.exclude_align > 0 then if is_float_type t.vtype then "decimal (unaligned)" else "whole number (unaligned)"
      else (
        match t.vtype with
        | "i32" | "u32" -> "whole number"
        | "i16" | "u16" -> "whole number (2-byte)"
        | "i64" | "u64" -> "whole number (8-byte)"
        | "i8" | "u8" -> "whole number (1-byte)"
        | "f32" -> "decimal"
        | _ -> "decimal (8-byte)")

let encode t (v : Value.t) : (Helper_api.value, string) result =
  let no_fit () = Error (Printf.sprintf "%s doesn't fit in a %s" (text_of v) (describe t)) in
  match t.encoding with
  | Plain when is_float_type t.vtype -> Ok (Vfloat (Value.to_float v))
  | Plain -> (
      match v with
      | Value.Dec d -> Error (Printf.sprintf "this value is a whole number, and %s has a decimal part" d.text)
      | Value.Int n -> if fits t.vtype n then Ok (Vint n) else no_fit ())
  | Scaled k -> (
      match scale_value v k with
      | Ok p when fits t.vtype p -> Ok (Vint p)
      | Error `Inexact -> Error (Printf.sprintf "%s has more decimal places than this value keeps" (text_of v))
      | _ -> no_fit ())
  | Fraction -> Ok (Vfloat (Value.to_float v /. 100.))

let trim_zeros s =
  if String.contains s '.' then (
    let n = ref (String.length s) in
    while !n > 0 && s.[!n - 1] = '0' do decr n done;
    if !n > 0 && s.[!n - 1] = '.' then decr n;
    String.sub s 0 !n)
  else s

let float_str f =
  if Float.is_nan f then "nan"
  else if f = Float.infinity then "inf"
  else if f = Float.neg_infinity then "-inf"
  else if f = 0. then "0"
  else
    let a = Float.abs f in
    let plain () =
      let s = trim_zeros (if a >= 1e6 then Printf.sprintf "%.2f" f else Printf.sprintf "%.10f" f) in
      if s = "-0" || s = "" then "0" else s
    in
    if a >= 1e-4 && a < 1e6 then (
      let s = Printf.sprintf "%.6g" f in
      if String.contains s 'e' then plain () else s)
    else plain ()

let decode t (x : Helper_api.value) =
  match (t.encoding, x) with
  | Scaled k, Vint n ->
      let p = digits_of_scale k in
      let neg = n < 0L in
      let a = if neg then Int64.neg n else n in
      let kk = Int64.of_int k in
      let ip = Int64.unsigned_div a kk and fp = Int64.unsigned_rem a kk in
      Printf.sprintf "%s%Lu.%0*Ld" (if neg then "-" else "") ip p fp
  | Scaled k, Vfloat f -> float_str (f /. float_of_int k)
  | Fraction, Vfloat f -> float_str (f *. 100.)
  | Fraction, Vint n -> Int64.to_string n
  | Plain, Vfloat f -> float_str f
  | Plain, Vint n -> if t.vtype = "u64" then Printf.sprintf "%Lu" n else Int64.to_string n
