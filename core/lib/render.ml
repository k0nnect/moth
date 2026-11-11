let commas n =
  let digits = string_of_int (abs n) in
  let len = String.length digits in
  let b = Buffer.create (len + (len / 3) + 1) in
  if n < 0 then Buffer.add_char b '-';
  String.iteri
    (fun i c ->
      if i > 0 && (len - i) mod 3 = 0 then Buffer.add_char b ',';
      Buffer.add_char b c)
    digits;
  Buffer.contents b

let dead_end = "nothing left that fits. that step was not applied, so you can try a different number."

let counts (o : Session.outcome) =
  let total = if List.mem Session.Showing_wide o.notes then o.wide else o.main in
  let head = Printf.sprintf "%s %s" (commas total) (if total = 1 then "match" else "matches") in
  match List.filter (fun (_, c) -> c > 0) o.by_tag with
  | [] -> head
  | tags -> Printf.sprintf "%s (%s)" head (String.concat ", " (List.map (fun (tag, c) -> commas c ^ " " ^ tag) tags))

let went = function Session.Down -> "went down" | Up -> "went up" | Same -> "stayed the same" | Changed -> "changed"

(* position of a note's line under the counts line *)
let rank = function Session.Fell_back _ -> 0 | Showing_wide -> 1 | Untracked -> 2 | Lost _ -> 3

let note_line = function
  | Session.Fell_back r -> Printf.sprintf "no exact match for that number. kept the ones that %s instead." (went r)
  | Showing_wide -> "nothing matched as a plain number. showing other ways it could be stored."
  | Untracked -> "some rarer forms are too common to track yet. they'll be rechecked on your next number."
  | Lost n -> Printf.sprintf "%s matches disappeared because the game freed that memory." (commas n)

let outcome (o : Session.outcome) =
  if o.dead_end then dead_end
  else
    let notes = List.stable_sort (fun a b -> compare (rank a) (rank b)) o.notes in
    String.concat "\n" (counts o :: List.map note_line notes)

let rows = function
  | [] -> "nothing to list yet. narrow it down with your next number."
  | rs ->
      let cells = List.map (fun (r : Session.row) -> (Printf.sprintf "#%d" r.n, r.shown, Printf.sprintf "0x%Lx" r.addr, r.tag)) rs in
      let width pick = List.fold_left (fun w c -> max w (String.length (pick c))) 0 cells in
      let wn = width (fun (n, _, _, _) -> n) and wv = width (fun (_, v, _, _) -> v) and wa = width (fun (_, _, a, _) -> a) in
      let pad w s = s ^ String.make (w - String.length s) ' ' in
      String.concat "\n" (List.map (fun (n, v, a, tag) -> String.concat "  " [ pad wn n; pad wv v; pad wa a; tag ]) cells)
