open Helper_api

type relation = Down | Up | Same | Changed
type note = Fell_back of relation | Showing_wide | Untracked | Lost of int

type outcome = {
  main : int;
  wide : int;
  by_tag : (string * int) list;
  notes : note list;
  dead_end : bool;
  ms : float;
  bytes : int;
  unreadable : int;
  groups : (group_info * Interp.tier) list;
}

type row = { n : int; addr : int64; interp_id : string; tag : string; shown : string }

(* one helper group and the interpretation it searches for *)
type group = { interp : Interp.t; info : group_info }

(* current: the hunt's groups in plan order. last: the last number typed that applied, which the
   helper's stored values hold; None after a relation step moved them on. *)
type hunt = { current : group list; last : Value.t option }

type listed = { at : int; li : Interp.t; laddr : int64 }

(* prev_bits: the bytes the write replaced, which undo puts back exactly *)
type write = { wn : int; wi : Interp.t; waddr : int64; prev : value; prev_bits : int64; next : value }

type written = { where : int64; wtype : string; bits_before : int64; bits_after : int64 }
type set_done = { text : string; w : written; held : bool }

type t = {
  h : helper;
  budget_mb : int option; (* sent on a find's scan *)
  settle : unit -> unit; (* the wait between a write and reading it back *)
  mutable hunt : hunt option;
  mutable listing : listed list; (* the last results listing, until the hunt changes *)
  mutable undo : write list; (* newest first *)
}

let create ?budget_mb ?(settle = fun () -> Unix.sleepf 0.25) h = { h; budget_mb; settle; hunt = None; listing = []; undo = [] }
let no_hunt = "no hunt in progress. start one with: moth find <number>"
let too_many = "too many matches to compare by change yet. type the new number instead."
let game_closed = "the game closed. attach again once it's running."
let helper_lost = "lost contact with moth's helper. attach again."
let hunt_lost = "the hunt was lost. start again with: moth find <number>"
let page = 10
let listable = 1000

exception Helper_failed of err

let call = function Ok x -> x | Error e -> raise (Helper_failed e)

(* Runs one step. A helper error ends it; losing the game or the helper also ends the hunt, and
   the writes, whose addresses mean nothing once attached again. A group the helper no longer
   has ends the hunt, which can't go on without it; the writes stay. *)
let guard t f =
  let drop_hunt () =
    t.hunt <- None;
    t.listing <- []
  in
  let clear () =
    drop_hunt ();
    t.undo <- []
  in
  try f () with
  | Helper_failed { code = "target_gone"; _ } ->
      clear ();
      Error game_closed
  | Helper_failed { code = "helper_lost"; _ } ->
      clear ();
      Error helper_lost
  | Helper_failed { code = "no_such_group"; _ } ->
      drop_hunt ();
      Error hunt_lost
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

let outcome_of ?fell_back ?(dead_end = false) tl gs =
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
    ms = tl.spent_ms;
    bytes = tl.read_bytes;
    unreadable = tl.unread_bytes;
    groups = List.map (fun g -> (g.info, g.interp.tier)) gs;
  }

let spec_of (i : Interp.t) pred : group_spec = { id = i.id; vtype = i.vtype; align = i.align; exclude_align = i.exclude_align; pred }
let updated infos g = match List.find_opt (fun (i : group_info) -> i.id = g.info.id) infos with Some info -> { g with info } | None -> g

(* a group a previous-value predicate can narrow *)
let comparable g = g.info.tracked && g.info.has_values && g.info.count > 0
let pred_of_relation = function Down -> Lt_prev | Up -> Gt_prev | Same -> Eq_prev | Changed -> Ne_prev

let relation_between (m : Value.t) (last : Value.t) =
  let c = match (m, last) with Int a, Int b -> Int64.compare a b | _ -> Float.compare (Value.to_float m) (Value.to_float last) in
  if c < 0 then Down else if c > 0 then Up else Same

let narrow t tl items =
  if items = [] then (false, [])
  else
    let applied, infos, st = call (t.h.narrow ~atomic:true items) in
    add tl st;
    (applied, infos)

let commit t current last =
  t.hunt <- Some { current; last };
  t.listing <- []

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
          let current =
            List.filter_map
              (fun ((interp : Interp.t), _) -> Option.map (fun info -> { interp; info }) (List.find_opt (fun (i : group_info) -> i.id = interp.id) infos))
              plan
          in
          commit t current (Some v);
          Ok (outcome_of tl current))

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
              (* when the number changed, a float reading must have moved too, or a decimal lying
                 between the two numbers stays; only a group with stored values can tell *)
              let moved = match hunt.last with Some last -> relation_between m last <> Same | None -> false in
              let exact g =
                match pred g with Some (Range r) when moved && g.info.has_values -> Some (Range { r with moved = true }) | p -> p
              in
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
              (* keep the groups [keep] picks, with their new counts; the rest leave the helper *)
              let finish ?fell_back keep infos =
                let current = List.map (updated infos) (List.filter keep hunt.current) in
                commit t current (Some m);
                let gone = List.filter_map (fun g -> if keep g then None else Some g.info.id) hunt.current in
                if gone <> [] then call (t.h.drop gone);
                Ok (outcome_of ?fell_back tl current)
              in
              if applied then finish fits (narrowed @ rescanned)
              else if List.exists (fun (i : group_info) -> i.tracked && i.count > 0) rescanned then
                (* only the rescan matched: the old tracked groups are out. A rescan that came back
                   untracked again holds only a count of how common the number is, not a match. *)
                finish (fun g -> fits g && not g.info.tracked) rescanned
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
                    if applied then finish ~fell_back:r fits (narrowed @ rescanned) else Ok (outcome_of ~dead_end:true tl hunt.current)))

let next_relation t r =
  match t.hunt with
  | None -> Error no_hunt
  | Some hunt -> (
      match List.filter comparable hunt.current with
      | [] when List.exists (fun g -> g.info.count > 0) hunt.current -> Error too_many
      | [] -> Ok (outcome_of ~dead_end:true (tally ()) hunt.current)
      | gs ->
          guard t (fun () ->
              let tl = tally () in
              let applied, narrowed = narrow t tl (List.map (fun g -> (g.info.id, pred_of_relation r)) gs) in
              if applied then (
                let current = List.map (updated narrowed) hunt.current in
                (* the stored values now hold what this step read, not the last number typed *)
                commit t current None;
                Ok (outcome_of tl current))
              else Ok (outcome_of ~dead_end:true tl hunt.current)))

let results t ~all =
  match t.hunt with
  | None -> Error no_hunt
  | Some hunt ->
      guard t (fun () ->
          let wide_too = all || showing_wide hunt.current in
          let listed g = g.info.tracked && g.info.count > 0 && g.info.count <= listable && (g.interp.tier = Interp.Main || wide_too) in
          let found =
            List.concat_map
              (fun g -> List.map (fun (it : item) -> (g.interp, it)) (call (t.h.peek ~id:g.info.id ~vtype:g.interp.vtype ~offset:0 ~limit:page)))
              (List.filter listed hunt.current)
          in
          let numbered = List.mapi (fun k (i, it) -> (k + 1, i, it)) found in
          t.listing <- List.map (fun (at, li, (it : item)) -> { at; li; laddr = it.addr }) numbered;
          Ok
            (List.map
               (fun (n, (i : Interp.t), (it : item)) -> { n; addr = it.addr; interp_id = i.id; tag = Interp.describe i; shown = Interp.decode i it.v })
               numbered))

let set t ~row text =
  match t.hunt with
  | None -> Error no_hunt
  | Some _ -> (
      match List.find_opt (fun l -> l.at = row) t.listing with
      | None -> Error (Printf.sprintf "no result #%d. run: moth results" row)
      | Some l -> (
          match Result.bind (Value.parse text) (Interp.encode l.li) with
          | Error e -> Error e
          | Ok next ->
              guard t (fun () ->
                  let vtype = l.li.vtype in
                  let r = call (t.h.write ~addr:l.laddr ~vtype (Put_value next)) in
                  t.undo <- { wn = row; wi = l.li; waddr = l.laddr; prev = r.prev; prev_bits = r.prev_bits; next } :: t.undo;
                  forget_last t;
                  let w = { where = l.laddr; wtype = vtype; bits_before = r.prev_bits; bits_after = Wire.bits_of_value ~vtype next } in
                  let text = Printf.sprintf "set #%d to %s (was %s)" row (Interp.decode l.li next) (Interp.decode l.li r.prev) in
                  (* a moment later, the raw bytes there say whether the game kept the write. A
                     read that fails says nothing: the write landed and stays recorded, and the
                     next command reports a closed game or a lost helper. *)
                  t.settle ();
                  let why =
                    match t.h.read ~addr:l.laddr ~vtype with
                    | Error _ -> None
                    | Ok v ->
                        let bits = Wire.bits_of_value ~vtype v in
                        if bits = w.bits_after then None
                        else if bits = r.prev_bits then
                          Some
                            (Printf.sprintf "the game put it straight back to %s. it's worked out from something else, so writing here won't stick."
                               (Interp.decode l.li r.prev))
                        else Some (Printf.sprintf "it already reads %s: the game keeps changing it." (Interp.decode l.li v))
                  in
                  match why with None -> Ok { text; w; held = true } | Some why -> Ok { text = text ^ "\n" ^ why; w; held = false })))

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
          Ok (Printf.sprintf "restored #%d to %s" w.wn (Interp.decode w.wi w.prev), done_))

let writes t = List.map (fun w -> (w.waddr, w.wi.vtype, w.prev, w.next)) t.undo

let current t = Option.map (fun h -> outcome_of (tally ()) h.current) t.hunt
