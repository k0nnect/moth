open Helper_api

let fail fmt = Printf.ksprintf failwith fmt

let request ~id ~op fields = Yojson.Safe.to_string (`Assoc (("id", `Int id) :: ("op", `String op) :: fields))

let pred_to_json = function
  | Eq n -> `Assoc [ ("op", `String "eq"); ("v", `String (Int64.to_string n)) ]
  | Range (lo, hi) -> `Assoc [ ("op", `String "range"); ("lo", `Float lo); ("hi", `Float hi) ]
  | Lt_prev -> `Assoc [ ("op", `String "lt_prev") ]
  | Gt_prev -> `Assoc [ ("op", `String "gt_prev") ]
  | Eq_prev -> `Assoc [ ("op", `String "eq_prev") ]
  | Ne_prev -> `Assoc [ ("op", `String "ne_prev") ]

let group_spec_to_json (g : group_spec) =
  `Assoc
    [
      ("id", `String g.id);
      ("type", `String g.vtype);
      ("align", `Int g.align);
      ("exclude_align", `Int g.exclude_align);
      ("pred", pred_to_json g.pred);
    ]

let addr_to_json a = `String (Printf.sprintf "0x%Lx" a)

let hex_digit c =
  match c with
  | '0' .. '9' -> Some (Char.code c - Char.code '0')
  | 'a' .. 'f' -> Some (Char.code c - Char.code 'a' + 10)
  | 'A' .. 'F' -> Some (Char.code c - Char.code 'A' + 10)
  | _ -> None

let addr_of_string s =
  let n = String.length s in
  if n < 3 || n > 18 || s.[0] <> '0' || s.[1] <> 'x' then fail "address %S is not a 0x hex string" s;
  let a = ref 0L in
  for i = 2 to n - 1 do
    match hex_digit s.[i] with
    | Some d -> a := Int64.logor (Int64.shift_left !a 4) (Int64.of_int d)
    | None -> fail "address %S is not a 0x hex string" s
  done;
  !a

let is_decimal s =
  let body = if String.length s > 0 && s.[0] = '-' then String.sub s 1 (String.length s - 1) else s in
  body <> "" && String.for_all (fun c -> c >= '0' && c <= '9') body

let value_to_json = function Vint n -> `String (Int64.to_string n) | Vfloat f -> `Float f

let value_of_json ~vtype (j : Yojson.Safe.t) =
  match (vtype, j) with
  | ("f32" | "f64"), `Float f -> Vfloat f
  | ("f32" | "f64"), `Int n -> Vfloat (float_of_int n)
  | ("f32" | "f64"), `String "nan" -> Vfloat Float.nan
  | ("f32" | "f64"), `String "inf" -> Vfloat Float.infinity
  | ("f32" | "f64"), `String "-inf" -> Vfloat Float.neg_infinity
  | ("f32" | "f64"), _ -> fail "a %s value must be a number, \"nan\", \"inf\" or \"-inf\"" vtype
  | ("i8" | "u8" | "i16" | "u16" | "i32" | "u32" | "i64" | "u64"), `String s -> (
      let parsed =
        if not (is_decimal s) then None
        else if vtype = "u64" then if s.[0] = '-' then None else Int64.of_string_opt ("0u" ^ s)
        else Int64.of_string_opt s
      in
      match parsed with Some n -> Vint n | None -> fail "%S is not a %s value" s vtype)
  | ("i8" | "u8" | "i16" | "u16" | "i32" | "u32" | "i64" | "u64"), _ -> fail "a %s value must be a decimal string" vtype
  | _ -> fail "unknown type %S" vtype

let width = function
  | "i8" | "u8" -> 1
  | "i16" | "u16" -> 2
  | "i32" | "u32" | "f32" -> 4
  | "i64" | "u64" | "f64" -> 8
  | vtype -> fail "unknown type %S" vtype

let bits_to_json ~vtype bits =
  let w = width vtype in
  let bits = if w = 8 then bits else Int64.logand bits (Int64.pred (Int64.shift_left 1L (8 * w))) in
  `String (Printf.sprintf "0x%0*Lx" (2 * w) bits)

let bits_of_value ~vtype v =
  let w = width vtype in
  let raw = match (v, vtype) with Vfloat f, "f32" -> Int64.of_int32 (Int32.bits_of_float f) | Vfloat f, _ -> Int64.bits_of_float f | Vint n, _ -> n in
  if w = 8 then raw else Int64.logand raw (Int64.pred (Int64.shift_left 1L (8 * w)))

let bits_of_json ~vtype (j : Yojson.Safe.t) =
  let digits = 2 * width vtype in
  let bad () = fail "a %s value's bytes must be a 0x hex string of up to %d digits" vtype digits in
  match j with
  | `String s when String.length s > 2 && String.length s <= 2 + digits && String.starts_with ~prefix:"0x" s ->
      let add a c = match hex_digit c with Some d -> Int64.logor (Int64.shift_left a 4) (Int64.of_int d) | None -> bad () in
      String.fold_left add 0L (String.sub s 2 (String.length s - 2))
  | _ -> bad ()

let field name (j : Yojson.Safe.t) =
  match j with
  | `Assoc fields -> ( match List.assoc_opt name fields with Some v -> v | None -> fail "missing %S" name)
  | _ -> fail "expected an object with %S" name

let int_field name j = match field name j with `Int n -> n | _ -> fail "%S must be an integer" name
let bool_field name j = match field name j with `Bool b -> b | _ -> fail "%S must be true or false" name
let string_field name j = match field name j with `String s -> s | _ -> fail "%S must be a string" name
let list_of what (j : Yojson.Safe.t) = match j with `List l -> l | _ -> fail "%s must be a list" what

let group_infos j =
  List.map
    (fun g -> { id = string_field "id" g; count = int_field "count" g; tracked = bool_field "tracked" g; has_values = bool_field "has_values" g })
    (list_of "groups" j)

let stats j =
  let ms = match field "ms" j with `Float f -> f | `Int n -> float_of_int n | _ -> fail "\"ms\" must be a number" in
  { regions = int_field "regions" j; bytes = int_field "bytes" j; unreadable_bytes = int_field "unreadable_bytes" j; lost = int_field "lost" j; ms }

let items ~vtype j =
  List.map (fun i -> { addr = addr_of_string (string_field "addr" i); v = value_of_json ~vtype (field "v" i) }) (list_of "items" j)

let parse_reply line =
  let bad message = Error { code = "bad_reply"; message } in
  match Yojson.Safe.from_string line with
  | exception Yojson.Json_error m -> bad m
  | `Assoc fields as reply -> (
      match (List.assoc_opt "ok" fields, List.assoc_opt "id" fields, List.assoc_opt "error" fields) with
      | Some (`Bool true), Some (`Int id), _ -> Ok (id, reply)
      | Some (`Bool true), _, _ -> bad "reply has no id"
      | Some (`Bool false), _, Some (`Assoc e) -> (
          match (List.assoc_opt "code" e, List.assoc_opt "message" e) with
          | Some (`String code), Some (`String message) -> Error { code; message }
          | _ -> bad "error reply without a code and message")
      | Some (`Bool false), _, _ -> bad "error reply without an error"
      | _ -> bad "reply has no ok")
  | _ -> bad "reply is not an object"
