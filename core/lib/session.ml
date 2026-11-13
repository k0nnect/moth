open Helper_api

type relation = Down | Up | Same | Changed
type note = Fell_back of relation | Showing_wide | Untracked | Lost of int | Snapshot_taken

type outcome = {
  main : int;
  wide : int;
  by_tag : (string * int) list;
  notes : note list;
  dead_end : bool;
  by_relation : bool;
  ms : float;
  bytes : int;
  unreadable : int;
  groups : (group_info * Interp.tier) list;
}

type row = { n : int; addr : int64; interp_id : string; tag : string; shown : string; away : int option }
type listing = { rows : row list; total : int; from : int }
type kept = { name : string; kaddr : int64; ktag : string; kshown : string; set_to : string option; put_back : bool }

(* one helper group and the interpretation it searches for *)
type group = { interp : Interp.t; info : group_info }

(* current: the hunt's groups in plan order. last: the last number typed that applied, which the
   helper's stored values hold; None after a relation step (a fallback is one) moved them on, and
   for a hunt from a snapshot. blind: a hunt from a snapshot no step has narrowed yet, every
   aligned place still a candidate. *)
type hunt = { current : group list; last : Value.t option; blind : bool }

(* a row of the latest listing, which set and keep_row take by its number *)
type listed = { at : int; li : Interp.t; laddr : int64; lshown : string }

(* wlabel: how the texts name it, "#2" or a kept name. prev_bits: the bytes the write replaced,
   which undo puts back exactly *)
type write = { wlabel : string; wi : Interp.t; waddr : int64; prev : value; prev_bits : int64; next : value }

(* a value kept by name: where it is, read and written as [ki] *)
type keeping = { kname : string; kat : int64; ki : Interp.t }

type written = { where : int64; wtype : string; bits_before : int64; bits_after : int64 }
type set_done = { text : string; w : written; held : bool option }

type t = {
  h : helper;
  budget_mb : int option; (* sent on a find's scan and on a snapshot *)
  snapshot_mb : int option; (* the most memory a snapshot may take, sent on each *)
  settle : unit -> unit; (* the wait between a write and reading it back *)
  mutable hunt : hunt option;
  mutable listing : listed list; (* the rows of the last results listing, until the hunt changes *)
  mutable undo : write list; (* newest first *)
  mutable named : keeping list; (* the values kept by name, in the order kept *)
}

let create ?budget_mb ?snapshot_mb ?(settle = fun () -> Unix.sleepf 0.25) h =
  { h; budget_mb; snapshot_mb; settle; hunt = None; listing = []; undo = []; named = [] }

let no_hunt = "no hunt in progress. start one with: moth find <number>, or with no number: moth snapshot"
let too_many = "too many matches to compare by change yet. type the new number instead, or start again by change alone: moth snapshot"
let game_closed = "the game closed. attach again once it's running."
let helper_lost = "lost contact with moth's helper. attach again."
let hunt_lost = "the hunt was lost. start again with: moth find <number>"
let out_of_memory = "moth's helper ran out of memory doing that. close something large, then try again."

let lost_values =
  "moth lost what it remembered of the last step, so it can't compare by change now. type the number it shows (moth next <number>), or start again: moth snapshot"

let no_row n = Printf.sprintf "no result #%d. run: moth results" n

(* a click names the address its row showed: a row that is anywhere else now is not that row *)
let moved ~row ~at (l : listed) =
  match at with
  | Some a when a <> l.laddr -> Some (Printf.sprintf "row %d is not at 0x%Lx any more: the list changed. look again: moth results" row a)
  | _ -> None
let no_kept name = Printf.sprintf "no kept value called %s. see: moth kept" name
let find_kept t name = List.find_opt (fun k -> k.kname = name) t.named

exception Helper_failed of err

let call = function Ok x -> x | Error e -> raise (Helper_failed e)

let drop_hunt t =
  t.hunt <- None;
  t.listing <- []

(* losing the game or the helper ends the hunt, the writes and the kept values, whose addresses
   mean nothing once attached again *)
let clear t =
  drop_hunt t;
  t.undo <- [];
  t.named <- []

(* Runs one step. A helper error ends it; losing the game or the helper also clears the session.
   A group the helper no longer has ends the hunt, which can't go on without it; the writes and
   the kept values stay. The helper running out of memory, or no longer holding the values a
   step compares with, leaves its positions as they were, so the hunt stays. *)
let guard t f =
  try f () with
  | Helper_failed { code = "target_gone"; _ } ->
      clear t;
      Error game_closed
  | Helper_failed { code = "helper_lost"; _ } ->
      clear t;
      Error helper_lost
  | Helper_failed { code = "no_such_group"; _ } ->
      drop_hunt t;
      Error hunt_lost
  | Helper_failed { code = "no_memory"; _ } -> Error out_of_memory
  | Helper_failed { code = "no_values"; _ } -> Error lost_values
  | Helper_failed e -> Error (if e.message = "" then e.code else e.message)

(* the helper's time, bytes read, bytes it could not read and lost candidates over one step *)
type tally = { mutable spent_ms : float; mutable read_bytes : int; mutable unread_bytes : int; mutable lost_count : int }

let tally () = { spent_ms = 0.; read_bytes = 0; unread_bytes = 0; lost_count = 0 }

let add tl (s : stats) =
  tl.spent_ms <- tl.spent_ms +. s.ms;
  tl.read_bytes <- tl.read_bytes + s.bytes;
  tl.unread_bytes <- tl.unread_bytes + s.unreadable_bytes;
  tl.lost_count <- tl.lost_count + s.lost

let total tier gs = List.fold_left (fun a g -> if g.info.tracked && g.interp.tier = tier then a + g.info.count else a) 0 gs

let showing_wide gs = total Interp.Main gs = 0 && total Interp.Wide gs > 0

let outcome_of ?fell_back ?(dead_end = false) ?(by_relation = false) tl gs =
  let wide_shown = showing_wide gs in
  let shown = if wide_shown then Interp.Wide else Interp.Main in
  let by_tag =
    List.fold_left
      (fun acc g ->
        if g.info.tracked && g.interp.tier = shown && g.info.count > 0 then
          let tag = Interp.describe g.interp in
          if List.mem_assoc tag acc then List.map (fun (k, c) -> if k = tag then (k, c + g.info.count) else (k, c)) acc
          else acc @ [ (tag, g.info.count) ]
        else acc)
      [] gs
  in
  let notes =
    (match fell_back with Some r -> [ Fell_back r ] | None -> [])
    @ (if wide_shown then [ Showing_wide ] else [])
    @ (if List.exists (fun g -> not g.info.tracked) gs then [ Untracked ] else [])
    @ if (not dead_end) && tl.lost_count > 0 then [ Lost tl.lost_count ] else []
  in
  {
    main = total Interp.Main gs;
    wide = total Interp.Wide gs;
    by_tag;
    notes;
    dead_end;
    by_relation;
    ms = tl.spent_ms;
    bytes = tl.read_bytes;
    unreadable = tl.unread_bytes;
    groups = List.map (fun g -> (g.info, g.interp.tier)) gs;
  }

let spec_of (i : Interp.t) pred : group_spec = { id = i.id; vtype = i.vtype; align = i.align; exclude_align = i.exclude_align; pred }
let updated infos g = match List.find_opt (fun (i : group_info) -> i.id = g.info.id) infos with Some info -> { g with info } | None -> g

(* a group a previous-value predicate can narrow *)
let comparable_info (i : group_info) = i.tracked && i.has_values && i.count > 0
let comparable g = comparable_info g.info
let can_compare (o : outcome) = List.exists (fun (i, _) -> comparable_info i) o.groups
let pred_of_relation = function Down -> Lt_prev | Up -> Gt_prev | Same -> Eq_prev | Changed -> Ne_prev

let relation_between (m : Value.t) (last : Value.t) =
  let c = match (m, last) with Int a, Int b -> Int64.compare a b | _ -> Float.compare (Value.to_float m) (Value.to_float last) in
  if c < 0 then Down else if c > 0 then Up else Same

(* range [a] lies within range [b] *)
let within (a : range) (b : range) = b.lo <= a.lo && a.hi <= b.hi

let narrow t tl items =
  if items = [] then (false, [])
  else
    let applied, infos, st = call (t.h.narrow ~atomic:true items) in
    add tl st;
    (applied, infos)

(* the hunt is [current] now; a step that applied leaves it blind no longer *)
let commit ?(blind = false) t current last =
  t.hunt <- Some { current; last; blind };
  t.listing <- []

(* the groups the helper reports for the interpretations, in their order *)
let groups_of interps infos =
  List.filter_map
    (fun (interp : Interp.t) -> Option.map (fun info -> { interp; info }) (List.find_opt (fun (i : group_info) -> i.id = interp.id) infos))
    interps

(* a write or an undo changed a stored value, so the last number typed no longer says which way
   a later number went *)
let forget_last t = match t.hunt with Some hunt -> t.hunt <- Some { hunt with last = None } | None -> ()

let find t text =
  match Value.parse text with
  | Error e -> Error e
  | Ok v ->
      guard t (fun () ->
          let plan = List.filter_map (fun i -> Option.map (fun p -> (i, p)) (Interp.pred_for i v)) (Interp.plan v) in
          let tl = tally () in
          let infos, st =
            match t.h.scan ~keep:false ~budget_mb:t.budget_mb (List.map (fun (i, p) -> spec_of i p) plan) with
            | Ok r -> r
            | Error e ->
                (* the helper may have dropped the old groups already, so the old hunt is over *)
                t.hunt <- None;
                t.listing <- [];
                raise (Helper_failed e)
          in
          add tl st;
          let current = groups_of (List.map fst plan) infos in
          commit t current (Some v);
          Ok (outcome_of tl current))

(* ---- a hunt from a snapshot ---- *)

let size n =
  let f = float_of_int n in
  let kb = max 1 (int_of_float (Float.round (f /. 1024.))) in
  let mb = int_of_float (Float.round (f /. 1048576.)) in
  if kb < 1024 then Printf.sprintf "%d kb" kb else if mb < 1024 then Printf.sprintf "%d mb" mb else Printf.sprintf "%.1f gb" (f /. 1073741824.)

(* the helper's size check refused with "<bytes> to snapshot, limit <bytes>": the two counts *)
let refused_sizes message =
  let count s = if s <> "" && String.for_all (fun c -> c >= '0' && c <= '9') s then int_of_string_opt s else None in
  match String.split_on_char ' ' message with
  | [ need; "to"; "snapshot,"; "limit"; limit ] -> ( match (count need, count limit) with Some n, Some l -> Some (n, l) | _ -> None)
  | _ -> None

(* the daemon reads MOTH_SNAPSHOT_MB once, when it starts *)
let over_limit need limit =
  Printf.sprintf
    "the game has %s of memory to snapshot, more than moth's limit of %s. the limit is set when moth starts: moth stop, then run moth again with MOTH_SNAPSHOT_MB=<mb> (stopping forgets the hunt and the kept values). or hunt with a number: moth find <number>"
    (size need) (size limit)

let no_memory = "moth couldn't get the memory for a snapshot. close something large, or hunt with a number: moth find <number>"

let snapshot t =
  guard t (fun () ->
      let groups = List.map (fun (i : Interp.t) -> (i.id, i.vtype, i.align)) Interp.blind_plan in
      match t.h.snapshot ~cap_mb:t.snapshot_mb ~budget_mb:t.budget_mb groups with
      | Error { code = "too_big"; message } -> (
          match refused_sizes message with
          | Some (need, limit) ->
              (* the size check refused before the helper changed anything: the hunt there was stays *)
              Error (over_limit need limit)
          | None ->
              (* it couldn't get the memory and has let go of its groups (a message without the
                 two sizes is taken the same way), so the hunt there was is over *)
              drop_hunt t;
              Error no_memory)
      | Error e ->
          (* the helper may have dropped the old groups already, as in a find *)
          drop_hunt t;
          raise (Helper_failed e)
      | Ok (infos, st) ->
          let tl = tally () in
          add tl st;
          let current = groups_of Interp.blind_plan infos in
          commit ~blind:true t current None;
          let o = outcome_of tl current in
          Ok { o with notes = Snapshot_taken :: o.notes })

let next_number t text =
  match t.hunt with
  | None -> Error no_hunt
  | Some hunt -> (
      match Value.parse text with
      | Error e -> Error e
      | Ok m ->
          guard t (fun () ->
              let tl = tally () in
              let pred g = Interp.pred_for g.interp m in
              let fits g = pred g <> None in
              (* when the ranges the two numbers give a float reading overlap only in part, or not
                 at all, the value must have moved too, or a decimal lying between the two numbers
                 stays. A range that holds the other is a more (or less) exact number for the same
                 value, which need not have moved. Only a group with stored values can tell. *)
              let moved g =
                match hunt.last with
                | Some last when g.info.has_values -> (
                    match (pred g, Interp.pred_for g.interp last) with
                    | Some (Range a), Some (Range b) -> not (within a b || within b a)
                    | _ -> false)
                | _ -> false
              in
              let exact g = match pred g with Some (Range r) when moved g -> Some (Range { r with moved = true }) | p -> p in
              (* the exact number in the tracked groups, all or nothing *)
              let applied, narrowed =
                narrow t tl
                  (List.filter_map (fun g -> if g.info.tracked && g.info.count > 0 then Option.map (fun p -> (g.info.id, p)) (exact g) else None) hunt.current)
              in
              (* the untracked groups, searched again for it *)
              let rescan = List.filter_map (fun g -> if g.info.tracked then None else Option.map (spec_of g.interp) (pred g)) hunt.current in
              let rescanned =
                if rescan = [] then []
                else
                  let infos, st = call (t.h.scan ~keep:true ~budget_mb:None rescan) in
                  add tl st;
                  infos
              in
              (* keep the groups [keep] picks, with their new counts; the rest leave the helper.
                 [last]: the number the stored values now hold, if any *)
              let finish ?fell_back ~last keep infos =
                let current = List.map (updated infos) (List.filter keep hunt.current) in
                commit t current last;
                let gone = List.filter_map (fun g -> if keep g then None else Some g.info.id) hunt.current in
                if gone <> [] then call (t.h.drop gone);
                Ok (outcome_of ?fell_back tl current)
              in
              if applied then finish ~last:(Some m) fits (narrowed @ rescanned)
              else if List.exists (fun (i : group_info) -> i.tracked && i.count > 0) rescanned then
                (* only the rescan matched: the old tracked groups are out. A rescan that came back
                   untracked again holds only a count of how common the number is, not a match. *)
                finish ~last:(Some m) (fun g -> fits g && not g.info.tracked) rescanned
              else
                match hunt.last with
                | None ->
                    (* no number the stored values hold, so no direction to fall back on *)
                    Ok (outcome_of ~dead_end:true tl hunt.current)
                | Some last ->
                    let r = relation_between m last in
                    let applied, narrowed =
                      narrow t tl (List.filter_map (fun g -> if fits g && comparable g then Some (g.info.id, pred_of_relation r) else None) hunt.current)
                    in
                    (* a fallback is a relation step: the stored values hold what it read, not the
                       number typed, so it clears the baseline as next_relation does *)
                    if applied then finish ~fell_back:r ~last:None fits (narrowed @ rescanned) else Ok (outcome_of ~dead_end:true tl hunt.current)))

let next_relation t r =
  match t.hunt with
  | None -> Error no_hunt
  | Some hunt -> (
      match List.filter comparable hunt.current with
      | [] when List.exists (fun g -> g.info.count > 0) hunt.current -> Error too_many
      | [] -> Ok (outcome_of ~dead_end:true ~by_relation:true (tally ()) hunt.current)
      | gs ->
          guard t (fun () ->
              let tl = tally () in
              let applied, narrowed = narrow t tl (List.map (fun g -> (g.info.id, pred_of_relation r)) gs) in
              if applied then (
                let current = List.map (updated narrowed) hunt.current in
                (* the stored values now hold what this step read, not the last number typed *)
                commit t current None;
                Ok (outcome_of ~by_relation:true tl current))
              else Ok (outcome_of ~dead_end:true ~by_relation:true tl hunt.current)))

let blind t = match t.hunt with Some h -> h.blind | None -> false

(* ---- results: pages, and nearness to a value ---- *)

let reach = 4096
let peek_cap = 1000 (* the most items the helper gives for one peek *)
let near_page = 64 (* the items asked for at a time near a value *)
let past_end from total = Printf.sprintf "no result #%d. there are %d." from total

(* the tracked groups with matches in the tier shown (main; wide too under [all] or when only
   wide is left), in plan order *)
let shown_groups hunt ~all =
  let wide_too = all || showing_wide hunt.current in
  List.filter (fun g -> g.info.tracked && g.info.count > 0 && (g.interp.tier = Interp.Main || wide_too)) hunt.current

(* [want] of the group's positions from index [offset], in address order, in as many peeks as
   the helper needs for them. A position that can't be read comes as a hole in its place, so a
   short reply means the group has no more. *)
let peek_span t g ~offset ~want =
  let rec go offset want =
    let limit = min want peek_cap in
    if limit <= 0 then []
    else
      let items = call (t.h.peek ~id:g.info.id ~vtype:g.interp.vtype ~offset ~limit) in
      if List.length items < limit then items else items @ go (offset + limit) (want - limit)
  in
  go offset want

(* rows [from] to [from + limit - 1] of the groups, fewest matches first, ties in plan order,
   each group's in address order; only groups with rows in that span are asked. The rows as
   (number, interpretation, item, distance), and how many there are in all. *)
let paged t gs ~from ~limit =
  let gs = List.stable_sort (fun a b -> compare a.info.count b.info.count) gs in
  let total = List.fold_left (fun a g -> a + g.info.count) 0 gs in
  if total > 0 && from > total then Error (past_end from total)
  else
    let last = from + limit - 1 in
    let _, found =
      List.fold_left
        (fun (before, found) g ->
          (* this group's rows are numbered before + 1 to before + count *)
          let first = max from (before + 1) and upto = min last (before + g.info.count) in
          let found =
            if first > upto then found
            else found @ List.mapi (fun k it -> (first + k, g.interp, it, None)) (peek_span t g ~offset:(first - before - 1) ~want:(upto - first + 1))
          in
          (before + g.info.count, found))
        (0, []) gs
    in
    Ok (found, total)

(* a 0x address, any case *)
let parse_addr s =
  let n = String.length s in
  let hex c = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') in
  if n > 2 && s.[0] = '0' && (s.[1] = 'x' || s.[1] = 'X') && String.for_all hex (String.sub s 2 (n - 2)) then Int64.of_string_opt ("0x" ^ String.sub s 2 (n - 2))
  else None

(* [a]'s signed distance in bytes from [target] when it is within [reach]. Addresses are
   unsigned, so this holds near 0 and near the top of memory. *)
let distance ~target a =
  let r = Int64.of_int reach in
  if Int64.unsigned_compare a target >= 0 then
    let d = Int64.sub a target in
    if Int64.unsigned_compare d r <= 0 then Some (Int64.to_int d) else None
  else
    let d = Int64.sub target a in
    if Int64.unsigned_compare d r <= 0 then Some (-Int64.to_int d) else None

(* the group's positions within [reach] of [target], with their distances: from [reach] below
   (or 0), [near_page] at a time, up to the first one more than [reach] above *)
let positions_near t g target =
  let r = Int64.of_int reach in
  let beyond (it : item) = Int64.unsigned_compare it.addr target > 0 && distance ~target it.addr = None in
  let rec upto = function [] -> [] | it :: _ when beyond it -> [] | it :: rest -> it :: upto rest in
  let rec go from =
    let items = call (t.h.peek_from ~id:g.info.id ~vtype:g.interp.vtype ~addr:from ~limit:near_page) in
    let taken = upto items in
    let found = List.filter_map (fun (it : item) -> Option.map (fun d -> (d, it)) (distance ~target it.addr)) taken in
    match List.rev taken with
    | (last : item) :: _ when List.length taken = near_page && last.addr <> -1L -> found @ go (Int64.succ last.addr)
    | _ -> found
  in
  go (if Int64.unsigned_compare target r < 0 then 0L else Int64.sub target r)

(* every position of the groups within [reach] of [addr], nearest first, ties in plan order,
   numbered from 1; rows [from] to [from + limit - 1] of them, and how many there are *)
let nearby t gs ~target ~addr ~from ~limit =
  let all = List.concat (List.mapi (fun p g -> List.map (fun (d, it) -> (p, d, g.interp, it)) (positions_near t g addr)) gs) in
  let order (p, d, _, _) (q, e, _, _) = match compare (abs d) (abs e) with 0 -> compare p q | c -> c in
  let numbered = List.mapi (fun k (_, d, i, it) -> (k + 1, i, it, Some d)) (List.stable_sort order all) in
  let total = List.length numbered in
  if total = 0 then Error (Printf.sprintf "nothing within %d kb of %s." (reach / 1024) target)
  else if from > total then Error (past_end from total)
  else Ok (List.filter (fun (n, _, _, _) -> n >= from && n < from + limit) numbered, total)

(* how a value reads as [i]; "?" when it couldn't be read *)
let shown_as i = function Some x -> Interp.decode i x | None -> "?"

(* the page as rows; they become the rows set and keep_row take *)
let listing_of t ~from ~total found =
  t.listing <- List.map (fun (n, li, (it : item), _) -> { at = n; li; laddr = it.addr; lshown = shown_as li it.v }) found;
  let row (n, (i : Interp.t), (it : item), away) = { n; addr = it.addr; interp_id = i.id; tag = Interp.describe i; shown = shown_as i it.v; away } in
  { rows = List.map row found; total; from }

(* where a near target is: a 0x address, else a kept value's *)
let target_addr t target = match parse_addr target with Some a -> Some a | None -> Option.map (fun k -> k.kat) (find_kept t target)

let nothing_yet = "nothing to list yet. tell moth how the value moved first: moth next down, up, same or changed."

let results t ~all ~from ~limit ~near =
  match t.hunt with
  | None -> Error no_hunt
  | Some { blind = true; _ } -> Error nothing_yet
  | Some hunt -> (
      let gs = shown_groups hunt ~all in
      let list numbered = guard t (fun () -> Result.map (fun (found, total) -> listing_of t ~from ~total found) (numbered ())) in
      match near with
      | None -> list (fun () -> paged t gs ~from ~limit)
      | Some target -> (
          match target_addr t target with
          | None -> Error (no_kept target)
          | Some addr -> list (fun () -> nearby t gs ~target ~addr ~from ~limit)))

(* ---- writes ---- *)

(* writes the number at [addr] as [i] and records it for undo; then, a moment later, the raw
   bytes there say whether the game kept it. [label] names it in the texts: "#2", or a kept
   value's name. *)
let write_checked t ~label (i : Interp.t) addr text =
  match Result.bind (Value.parse text) (Interp.encode i) with
  | Error e -> Error e
  | Ok next ->
      guard t (fun () ->
          let vtype = i.vtype in
          let r = call (t.h.write ~addr ~vtype (Put_value next)) in
          let made = { wlabel = label; wi = i; waddr = addr; prev = r.prev; prev_bits = r.prev_bits; next } in
          t.undo <- made :: t.undo;
          forget_last t;
          let w = { where = addr; wtype = vtype; bits_before = r.prev_bits; bits_after = Wire.bits_of_value ~vtype next } in
          let text = Printf.sprintf "set %s to %s (was %s)" label (Interp.decode i next) (Interp.decode i r.prev) in
          t.settle ();
          (* held: read back as written, or not; None when it couldn't be read back. The write
             landed either way, so it is reported; a read-back that lost the game or the helper
             clears the session there, as any step's would. *)
          let held, why =
            match t.h.read ~addr ~vtype with
            | Error { code = "target_gone"; _ } ->
                clear t;
                (None, Some "the game closed right after, so moth couldn't check it.")
            | Error { code = "helper_lost"; _ } ->
                clear t;
                (None, Some "moth lost contact with its helper right after, so it couldn't check it.")
            | Error _ -> (None, Some "moth couldn't read it back to check.")
            | Ok v ->
                let bits = Wire.bits_of_value ~vtype v in
                if bits = w.bits_after then (Some true, None)
                else if bits = r.prev_bits then
                  ( Some false,
                    Some
                      (Printf.sprintf "the game put it straight back to %s. it's worked out from something else, so writing here won't stick."
                         (Interp.decode i r.prev)) )
                else (Some false, Some (Printf.sprintf "it already reads %s: the game keeps changing it." (Interp.decode i v)))
          in
          Ok { text = (match why with Some why -> text ^ "\n" ^ why | None -> text); w; held })

let set ?at t ~row text =
  match t.hunt with
  | None -> Error no_hunt
  | Some _ -> (
      match List.find_opt (fun l -> l.at = row) t.listing with
      | None -> Error (no_row row)
      | Some l when moved ~row ~at l <> None -> Error (Option.get (moved ~row ~at l))
      | Some l -> write_checked t ~label:(Printf.sprintf "#%d" row) l.li l.laddr text)

let undo t =
  match t.undo with
  | [] -> Error "nothing to undo"
  | w :: rest ->
      guard t (fun () ->
          let vtype = w.wi.vtype in
          let r = call (t.h.write ~addr:w.waddr ~vtype (Put_bits w.prev_bits)) in
          t.undo <- rest;
          forget_last t;
          let done_ = { where = w.waddr; wtype = vtype; bits_before = r.prev_bits; bits_after = w.prev_bits } in
          Ok (Printf.sprintf "restored %s to %s" w.wlabel (Interp.decode w.wi w.prev), done_))

let writes t = List.map (fun w -> (w.waddr, w.wi.vtype, w.prev, w.next)) t.undo

(* ---- values kept by name ---- *)

let max_kept = 64
let bad_name = "a name is lowercase letters, digits, - and _, starting with a letter."

(* a lowercase letter, then up to 23 lowercase letters, digits, _ or - *)
let valid_name s =
  let lower c = c >= 'a' && c <= 'z' in
  let n = String.length s in
  n >= 1 && n <= 24 && lower s.[0] && String.for_all (fun c -> lower c || (c >= '0' && c <= '9') || c = '_' || c = '-') s

(* the name a new kept value gets: the one given, or the first of v1, v2, ... not in use *)
let new_name t name =
  match name with
  | Some n when not (valid_name n) -> Error bad_name
  | Some n when find_kept t n <> None -> Error (Printf.sprintf "there's already a kept value called %s. pick another, or: moth forget %s" n n)
  | _ when List.length t.named >= max_kept -> Error (Printf.sprintf "that's %d kept values, the most moth holds. forget one first." max_kept)
  | Some n -> Ok n
  | None ->
      let rec free k = if find_kept t (Printf.sprintf "v%d" k) = None then Printf.sprintf "v%d" k else free (k + 1) in
      Ok (free 1)

let add_kept t name i addr = t.named <- t.named @ [ { kname = name; kat = addr; ki = i } ]

(* the value at [addr] as [i]'s type, or [None] when that memory can't be read *)
let read_as t (i : Interp.t) addr =
  match t.h.read ~addr ~vtype:i.vtype with
  | Ok v -> Some v
  | Error { code = "read_failed"; _ } -> None
  | Error e -> raise (Helper_failed e)

let keep_row ?at t ~row ~name =
  match new_name t name with
  | Error e -> Error e
  | Ok name -> (
      match t.hunt with
      | None -> Error no_hunt
      | Some _ -> (
          match List.find_opt (fun l -> l.at = row) t.listing with
          | None -> Error (no_row row)
          | Some l when moved ~row ~at l <> None -> Error (Option.get (moved ~row ~at l))
          | Some l ->
              guard t (fun () ->
                  (* how it reads now, which a write since the listing may have changed; as listed
                     when it can't be read *)
                  let shown = match read_as t l.li l.laddr with Some v -> Interp.decode l.li v | None -> l.lshown in
                  add_kept t name l.li l.laddr;
                  Ok (Printf.sprintf "kept #%d as %s (%s, %s)" row name shown (Interp.describe l.li)))))

let keep_at t ~addr text ~name =
  match new_name t name with
  | Error e -> Error e
  | Ok name -> (
      match Value.parse text with
      | Error e -> Error e
      | Ok v ->
          guard t (fun () ->
              let holding i = match read_as t i addr with Some x when Interp.holds i v x -> Some (i, x) | _ -> None in
              match List.find_map holding (Interp.plan v) with
              | None -> Error (Printf.sprintf "nothing at 0x%Lx reads as %s." addr text)
              | Some (i, x) ->
                  add_kept t name i addr;
                  Ok (Printf.sprintf "kept 0x%Lx as %s (%s, %s)" addr name (Interp.decode i x) (Interp.describe i))))

let kept t =
  guard t (fun () ->
      Ok
        (List.map
           (fun k ->
             let now = read_as t k.ki k.kat in
             (* its newest write: the newest still to undo at its address as its type, however it
                was made. It reads as it did before that write when the raw bytes there now are
                the ones it replaced and not the ones it wrote, as the write's own check reads
                them. *)
             let set_to, put_back =
               match List.find_opt (fun w -> w.waddr = k.kat && w.wi.vtype = k.ki.vtype) t.undo with
               | None -> (None, false)
               | Some w ->
                   let bits v = Wire.bits_of_value ~vtype:k.ki.vtype v in
                   (Some (Interp.decode k.ki w.next), match now with Some v -> bits v = w.prev_bits && bits v <> bits w.next | None -> false)
             in
             { name = k.kname; kaddr = k.kat; ktag = Interp.describe k.ki; kshown = shown_as k.ki now; set_to; put_back })
           t.named))

let forget t name =
  match find_kept t name with
  | None -> Error (no_kept name)
  | Some _ ->
      t.named <- List.filter (fun k -> k.kname <> name) t.named;
      Ok ("forgot " ^ name)

let set_kept t ~name text =
  match find_kept t name with
  | Some k -> write_checked t ~label:name k.ki k.kat text
  | None when parse_addr name <> None ->
      (* an address is changed through a kept value *)
      Error (Printf.sprintf "no kept value called %s. to change an address, keep it first: moth keep %s <the number it shows>" name name)
  | None -> Error (no_kept name)

let current t = Option.map (fun h -> outcome_of (tally ()) h.current) t.hunt
