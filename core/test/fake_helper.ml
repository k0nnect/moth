(* A scripted Helper_api.helper for tests: queue the replies each operation should give, then
   read back the calls the code under test made. An operation with nothing queued fails the
   test, except drop, which answers Ok () by default. *)
open Moth_core
open Helper_api

type call =
  | Scan of { keep : bool; specs : group_spec list }
  | Narrow of { atomic : bool; items : (string * pred) list }
  | Drop of string list
  | Peek of { id : string; offset : int; limit : int }
  | Write of { addr : int64; vtype : string; v : value }

type script = {
  mutable calls : call list; (* newest first *)
  scans : (group_info list * stats, err) result Queue.t;
  narrows : (bool * group_info list * stats, err) result Queue.t;
  drops : (unit, err) result Queue.t;
  peeks : (string, (item list, err) result Queue.t) Hashtbl.t; (* by group; "" answers any group *)
  writes : (value, err) result Queue.t;
}

let zero_stats = { regions = 0; bytes = 0; unreadable_bytes = 0; lost = 0; ms = 0. }

let next op q = match Queue.take_opt q with Some r -> r | None -> failwith (Printf.sprintf "fake helper: no %s reply queued" op)

let peek_queue f id =
  match Hashtbl.find_opt f.peeks id with
  | Some q -> q
  | None ->
      let q = Queue.create () in
      Hashtbl.replace f.peeks id q;
      q

let unscripted op = failwith (Printf.sprintf "fake helper: %s is not scripted" op)

let create () =
  let f =
    { calls = []; scans = Queue.create (); narrows = Queue.create (); drops = Queue.create (); peeks = Hashtbl.create 8; writes = Queue.create () }
  in
  let record c = f.calls <- c :: f.calls in
  let helper =
    {
      ps = (fun () -> unscripted "ps");
      modules = (fun _ -> unscripted "modules");
      attach = (fun _ -> unscripted "attach");
      scan =
        (fun ~keep specs ->
          record (Scan { keep; specs });
          next "scan" f.scans);
      narrow =
        (fun ~atomic items ->
          record (Narrow { atomic; items });
          next "narrow" f.narrows);
      drop =
        (fun ids ->
          record (Drop ids);
          match Queue.take_opt f.drops with Some r -> r | None -> Ok ());
      peek =
        (fun ~id ~offset ~limit ->
          record (Peek { id; offset; limit });
          let q = peek_queue f id in
          if Queue.is_empty q then next "peek" (peek_queue f "") else Queue.take q);
      read = (fun ~addr:_ ~vtype:_ -> unscripted "read");
      write =
        (fun ~addr ~vtype v ->
          record (Write { addr; vtype; v });
          next "write" f.writes);
      detach = (fun () -> unscripted "detach");
      close = (fun () -> ());
    }
  in
  (helper, f)

let queue_scan ?(stats = zero_stats) f infos = Queue.push (Ok (infos, stats)) f.scans
let queue_narrow ?(stats = zero_stats) f ~applied infos = Queue.push (Ok (applied, infos, stats)) f.narrows
let queue_peek f id items = Queue.push (Ok items) (peek_queue f id)
let queue_write f ~prev = Queue.push (Ok prev) f.writes

let queue_error f op (e : err) =
  match op with
  | "scan" -> Queue.push (Error e) f.scans
  | "narrow" -> Queue.push (Error e) f.narrows
  | "drop" -> Queue.push (Error e) f.drops
  | "peek" -> Queue.push (Error e) (peek_queue f "")
  | "write" -> Queue.push (Error e) f.writes
  | _ -> invalid_arg ("fake helper: cannot queue an error for " ^ op)

(* oldest first *)
let calls f = List.rev f.calls

let newest what pick f =
  match List.find_map pick f.calls with Some x -> x | None -> failwith ("fake helper: no " ^ what ^ " call was made")

let last_scan = newest "scan" (function Scan s -> Some (s.keep, s.specs) | _ -> None)
let last_scan_ids f = List.map (fun (s : group_spec) -> s.id) (snd (last_scan f))
let last_scan_keep f = fst (last_scan f)
let last_scan_spec f id = List.find (fun (s : group_spec) -> s.id = id) (snd (last_scan f))
let last_narrow = newest "narrow" (function Narrow n -> Some (n.atomic, n.items) | _ -> None)
let last_narrow_atomic f = fst (last_narrow f)
let last_narrow_ids f = List.map fst (snd (last_narrow f))

let last_narrow_pred f id =
  match List.assoc_opt id (snd (last_narrow f)) with
  | Some p -> p
  | None -> failwith ("fake helper: the last narrow did not list " ^ id)

let count pick f = List.length (List.filter pick f.calls)
let scan_calls = count (function Scan _ -> true | _ -> false)
let narrow_calls = count (function Narrow _ -> true | _ -> false)
let write_calls = count (function Write _ -> true | _ -> false)
let dropped f = List.concat_map (function Drop ids -> ids | _ -> []) (calls f)
let peeks f = List.filter_map (function Peek p -> Some (p.id, p.offset, p.limit) | _ -> None) (calls f)
let last_write = newest "write" (function Write w -> Some (w.addr, w.vtype, w.v) | _ -> None)
