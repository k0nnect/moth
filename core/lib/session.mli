(** The hunt: what to ask the helper for each number or change the user reports, and what
    came of it. The helper holds the candidates; this keeps the groups (one per interpretation
    from [Interp.plan], in plan order), the last number typed, the last results listing and the
    writes that can still be undone. *)

type t
type relation = Down | Up | Same | Changed
type note = Fell_back of relation | Showing_wide | Untracked | Lost of int

(** [main] and [wide]: matches in the tracked groups of each tier. [by_tag]: the tracked groups
    being shown (wide ones only under [Showing_wide]) summed per [Interp.describe] tag, in plan
    order, zero counts left out. A dead end reports the hunt as it stands, unchanged. [ms] and
    [bytes]: the helper's time and bytes read, summed over the calls the step made. [groups]: the
    hunt's groups in plan order. *)
type outcome = {
  main : int;
  wide : int;
  by_tag : (string * int) list;
  notes : note list;
  dead_end : bool;
  ms : float;
  bytes : int;
  groups : (Helper_api.group_info * Interp.tier) list;
}

type row = { n : int; addr : int64; interp_id : string; tag : string; shown : string }

val create : Helper_api.helper -> t

(** starts a new hunt *)
val find : t -> string -> (outcome, string) result

val next_number : t -> string -> (outcome, string) result
val next_relation : t -> relation -> (outcome, string) result

(** lists the hunt's smaller groups; [set] uses the latest listing until the hunt changes *)
val results : t -> all:bool -> (row list, string) result

val set : t -> row:int -> string -> (string, string) result

(** writes back the value the newest write replaced *)
val undo : t -> (string, string) result

(** the writes [undo] can still reverse: addr, vtype, previous, new; newest first *)
val writes : t -> (int64 * string * Helper_api.value * Helper_api.value) list

(** the hunt as it stands, or [None] when there is none: the counts, tags, groups and the notes
    the groups imply ([Showing_wide], [Untracked]); [ms] and [bytes] are 0 *)
val current : t -> outcome option
