type t = Int of int64 | Dec of { text : string; value : float; decimals : int }

let is_digits s = s <> "" && String.for_all (fun c -> c >= '0' && c <= '9') s

let parse input =
  let s = String.trim input in
  let s = String.concat "" (String.split_on_char ',' s) in
  let s = String.concat "" (String.split_on_char '_' s) in
  let neg = s <> "" && s.[0] = '-' in
  let body =
    if s <> "" && (s.[0] = '-' || s.[0] = '+') then String.sub s 1 (String.length s - 1) else s
  in
  let bad = Error (Printf.sprintf "'%s' isn't a number" (String.trim input)) in
  match String.split_on_char '.' body with
  | [ ip ] when is_digits ip -> (
      match Int64.of_string_opt ((if neg then "-" else "") ^ ip) with
      | Some n -> Ok (Int n)
      | None -> Error (Printf.sprintf "%s is too big" (String.trim input)))
  | [ ip; fp ] when is_digits ip && is_digits fp ->
      let text = (if neg then "-" else "") ^ ip ^ "." ^ fp in
      Ok (Dec { text; value = float_of_string text; decimals = String.length fp })
  | _ -> bad

let to_float = function Int n -> Int64.to_float n | Dec d -> d.value
