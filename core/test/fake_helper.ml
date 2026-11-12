(* A scripted Helper_api.helper for tests: queue the replies each operation should give, then
   read back the calls the code under test made. An operation with nothing queued fails the
   test, except drop, which answers Ok () by default. *)
open Moth_core
open Helper_api

type call =
  | Scan of { keep : bool; budget_mb : int option; specs : group_spec list }
  | Snapshot of { cap_mb : int option; budget_mb : int option; groups : (string * string * int) list }
  | Narrow of { atomic : bool; items : (string * pred) list }
  | Drop of string list
  | Peek of { id : string; vtype : string; offset : int; limit : int }
  | Peek_from of { id : string; vtype : string; addr : int64; limit : int }
  | Read of { addr : int64; vtype : string }
  | Write of { addr : int64; vtype : string; v : put }

type script = {
  mutable calls : call list; (* newest first *)
  scans : (group_info list * stats, err) result Queue.t;
  snapshots : (group_info list * stats, err) result Queue.t;
  narrows : (bool * group_info list * stats, err) result Queue.t;
  drops : (unit, err) result Queue.t;
  peeks : (string, (item list, err) result Queue.t) Hashtbl.t; (* by group; "" answers any group *)
  peek_froms : (string, (item list, err) result Queue.t) Hashtbl.t; (* the same *)
  reads : (value, err) result Queue.t;
  writes : (replaced, err) result Queue.t;
}

let zero_stats = { regions = 0; bytes = 0; unreadable_bytes = 0; lost = 0; ms = 0. }

let next op q = match Queue.take_opt q with Some r -> r | None -> failwith (Printf.sprintf "fake helper: no %s reply queued" op)

let group_queue tbl id =
  match Hashtbl.find_opt tbl id with
  | Some q -> q
  | None ->
      let q = Queue.create () in
      Hashtbl.replace tbl id q;
      q

let peek_queue f id = group_queue f.peeks id
let peek_from_queue f id = group_queue f.peek_froms id

(* the group's own reply, else one queued for any group *)
let next_for op q any = if Queue.is_empty q then next op any else Queue.take q

let unscripted op = failwith (Printf.sprintf "fake helper: %s is not scripted" op)

let create () =
  let f =
    { calls = []; scans = Queue.create (); snapshots = Queue.create (); narrows = Queue.create (); drops = Queue.create (); peeks = Hashtbl.create 8; peek_froms = Hashtbl.create 8; reads = Queue.create (); writes = Queue.create () }
  in
  let record c = f.calls <- c :: f.calls in
  let helper =
    {
      ps = (fun () -> unscripted "ps");
      modules = (fun _ -> unscripted "modules");
      attach = (fun _ -> unscripted "attach");
      scan =
        (fun ~keep ~budget_mb specs ->
          record (Scan { keep; budget_mb; specs });
          next "scan" f.scans);
      snapshot =
        (fun ~cap_mb ~budget_mb groups ->
          record (Snapshot { cap_mb; budget_mb; groups });
          next "snapshot" f.snapshots);
      narrow =
        (fun ~atomic items ->
          record (Narrow { atomic; items });
          next "narrow" f.narrows);
      drop =
        (fun ids ->
          record (Drop ids);
          match Queue.take_opt f.drops with Some r -> r | None -> Ok ());
      peek =
        (fun ~id ~vtype ~offset ~limit ->
          record (Peek { id; vtype; offset; limit });
          next_for "peek" (peek_queue f id) (peek_queue f ""));
      peek_from =
        (fun ~id ~vtype ~addr ~limit ->
          record (Peek_from { id; vtype; addr; limit });
          next_for "peek_from" (peek_from_queue f id) (peek_from_queue f ""));
      read =
        (fun ~addr ~vtype ->
          record (Read { addr; vtype });
          next "read" f.reads);
      write =
        (fun ~addr ~vtype v ->
          record (Write { addr; vtype; v });
          next "write" f.writes);
      status = (fun () -> unscripted "status");
      detach = (fun () -> unscripted "detach");
      close = (fun () -> ());
    }
  in
  (helper, f)

let queue_scan ?(stats = zero_stats) f infos = Queue.push (Ok (infos, stats)) f.scans
let queue_snapshot ?(stats = zero_stats) f infos = Queue.push (Ok (infos, stats)) f.snapshots
let queue_narrow ?(stats = zero_stats) f ~applied infos = Queue.push (Ok (applied, infos, stats)) f.narrows
let queue_peek f id items = Queue.push (Ok items) (peek_queue f id)
let queue_peek_from f id items = Queue.push (Ok items) (peek_from_queue f id)
let queue_read f v = Queue.push (Ok v) f.reads
let queue_write ?(prev_bits = 0L) f ~prev = Queue.push (Ok { prev; prev_bits }) f.writes

let queue_error f op (e : err) =
  match op with
  | "scan" -> Queue.push (Error e) f.scans
  | "snapshot" -> Queue.push (Error e) f.snapshots
  | "narrow" -> Queue.push (Error e) f.narrows
  | "drop" -> Queue.push (Error e) f.drops
  | "peek" -> Queue.push (Error e) (peek_queue f "")
  | "peek_from" -> Queue.push (Error e) (peek_from_queue f "")
  | "read" -> Queue.push (Error e) f.reads
  | "write" -> Queue.push (Error e) f.writes
  | _ -> invalid_arg ("fake helper: cannot queue an error for " ^ op)

(* oldest first *)
let calls f = List.rev f.calls

let newest what pick f =
  match List.find_map pick f.calls with Some x -> x | None -> failwith ("fake helper: no " ^ what ^ " call was made")

let last_scan = newest "scan" (function Scan s -> Some (s.keep, s.specs) | _ -> None)
let last_scan_ids f = List.map (fun (s : group_spec) -> s.id) (snd (last_scan f))
let last_scan_budget = newest "scan" (function Scan s -> Some s.budget_mb | _ -> None)
let last_scan_keep f = fst (last_scan f)
let last_scan_spec f id = List.find (fun (s : group_spec) -> s.id = id) (snd (last_scan f))
let last_snapshot = newest "snapshot" (function Snapshot s -> Some (s.cap_mb, s.budget_mb, s.groups) | _ -> None)
let last_snapshot_groups f = let _, _, g = last_snapshot f in g
let last_snapshot_cap f = let c, _, _ = last_snapshot f in c
let last_snapshot_budget f = let _, b, _ = last_snapshot f in b
let last_narrow = newest "narrow" (function Narrow n -> Some (n.atomic, n.items) | _ -> None)
let last_narrow_atomic f = fst (last_narrow f)
let last_narrow_ids f = List.map fst (snd (last_narrow f))

let last_narrow_pred f id =
  match List.assoc_opt id (snd (last_narrow f)) with
  | Some p -> p
  | None -> failwith ("fake helper: the last narrow did not list " ^ id)

let count pick f = List.length (List.filter pick f.calls)
let scan_calls = count (function Scan _ -> true | _ -> false)
let snapshot_calls = count (function Snapshot _ -> true | _ -> false)
let narrow_calls = count (function Narrow _ -> true | _ -> false)
let write_calls = count (function Write _ -> true | _ -> false)
let dropped f = List.concat_map (function Drop ids -> ids | _ -> []) (calls f)
let peeks f = List.filter_map (function Peek p -> Some (p.id, p.offset, p.limit) | _ -> None) (calls f)
let peek_vtypes f = List.filter_map (function Peek p -> Some (p.id, p.vtype) | _ -> None) (calls f)
let peek_froms f = List.filter_map (function Peek_from p -> Some (p.id, p.addr, p.limit) | _ -> None) (calls f)
let reads f = List.filter_map (function Read r -> Some (r.addr, r.vtype) | _ -> None) (calls f)

(* the group's newest peek: offset, limit *)
let last_peek f id = newest ("peek of " ^ id) (function Peek p when p.id = id -> Some (p.offset, p.limit) | _ -> None) f
let last_peek_from_addr f id = newest ("peek_from of " ^ id) (function Peek_from p when p.id = id -> Some p.addr | _ -> None) f
let last_write = newest "write" (function Write w -> Some (w.addr, w.vtype, w.v) | _ -> None)
