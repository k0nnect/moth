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
  | Lost n -> Printf.sprintf "%s %s disappeared because the game freed that memory." (commas n) (if n = 1 then "match" else "matches")

let size n =
  let f = float_of_int n in
  let kb = max 1 (int_of_float (Float.round (f /. 1024.))) in
  let mb = int_of_float (Float.round (f /. 1048576.)) in
  if kb < 1024 then Printf.sprintf "%d kb" kb else if mb < 1024 then Printf.sprintf "%d mb" mb else Printf.sprintf "%.1f gb" (f /. 1073741824.)

let unreadable_line n = Printf.sprintf "couldn't read %s of the game's memory, so matches there are missing." (size n)

let outcome (o : Session.outcome) =
  if o.dead_end then dead_end
  else
    let notes = List.stable_sort (fun a b -> compare (rank a) (rank b)) o.notes in
    let unread = if o.unreadable > 0 then [ unreadable_line o.unreadable ] else [] in
    String.concat "\n" ((counts o :: unread) @ List.map note_line notes)

(* lines of cells, two spaces apart, each column but the last padded to its widest cell *)
let table lines =
  let widths = List.fold_left (fun ws l -> List.map2 (fun w c -> max w (String.length c)) ws l) (List.map (fun _ -> 0) (List.hd lines)) lines in
  let pad w s = s ^ String.make (w - String.length s) ' ' in
  let line l =
    let last = List.length l - 1 in
    String.concat "  " (List.mapi (fun i (w, c) -> if i = last then c else pad w c) (List.combine widths l))
  in
  String.concat "\n" (List.map line lines)

let away = function Some 0 -> "0" | Some d -> Printf.sprintf "%+d" d | None -> ""

let rows = function
  | [] -> "nothing to list yet. narrow it down with your next number."
  | rs ->
      let near = List.exists (fun (r : Session.row) -> r.away <> None) rs in
      table
        (List.map
           (fun (r : Session.row) -> [ Printf.sprintf "#%d" r.n; r.shown; Printf.sprintf "0x%Lx" r.addr ] @ (if near then [ away r.away ] else []) @ [ r.tag ])
           rs)

let listing ?near ~all (l : Session.listing) =
  let text = rows l.rows in
  match (l.rows, List.rev l.rows) with
  | (first : Session.row) :: _, (last : Session.row) :: _ when last.n < l.total ->
      let again = (if all then " --all" else "") ^ Printf.sprintf " --from %d" (last.n + 1) ^ match near with Some t -> " --near " ^ t | None -> "" in
      Printf.sprintf "%s\nshowing %s-%s of %s. more: moth results%s" text (commas first.n) (commas last.n) (commas l.total) again
  | _ -> text

let kept = function
  | [] -> "nothing kept yet. keep a result with: moth keep <#n> [name]"
  | ks -> table (List.map (fun (k : Session.kept) -> [ k.name; k.kshown; Printf.sprintf "0x%Lx" k.kaddr; k.ktag ]) ks)
