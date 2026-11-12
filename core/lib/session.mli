(** The hunt: what to ask the helper for each number or change the user reports, and what
    came of it. The helper holds the candidates; this keeps the groups (one per interpretation
    from [Interp.plan], in plan order), the last number typed, the last results listing and the
    writes that can still be undone. *)

type t
type relation = Down | Up | Same | Changed
type note = Fell_back of relation | Showing_wide | Untracked | Lost of int

(** [main] and [wide]: matches in the tracked groups of each tier. [by_tag]: the tracked groups
    being shown (wide ones only under [Showing_wide]) summed per [Interp.describe] tag, in plan
    order, zero counts left out. A dead end reports the hunt as it stands, unchanged. [ms],
    [bytes] and [unreadable]: the helper's time, bytes read and bytes it could not read (scans
    only), summed over the calls the step made. [groups]: the hunt's groups in plan order. *)
type outcome = {
  main : int;
  wide : int;
  by_tag : (string * int) list;
  notes : note list;
  dead_end : bool;
  ms : float;
  bytes : int;
  unreadable : int;
  groups : (Helper_api.group_info * Interp.tier) list;
}

type row = { n : int; addr : int64; interp_id : string; tag : string; shown : string }

(** one write as it was made: where, as which type, the raw bytes it replaced and the ones it put
    there (see [Wire.bits_of_value]) *)
type written = { where : int64; wtype : string; bits_before : int64; bits_after : int64 }

(** what to tell the user, the write, and whether the address still held it a moment later
    ([true] when it could not be read back) *)
type set_done = { text : string; w : written; held : bool }

(** [budget_mb]: sent on a find's scan, none by default. [settle]: the wait between a write and
    reading it back, a quarter of a second by default. *)
val create : ?budget_mb:int -> ?settle:(unit -> unit) -> Helper_api.helper -> t

(** starts a new hunt. A scan that fails ends the hunt there was. *)
val find : t -> string -> (outcome, string) result

(** narrows to the number typed. When it differs from the last number typed, a float reading in
    a group with stored values must also have moved. With no exact match it falls back on the
    direction the number moved from the last number typed, unless a relation step, a set or an
    undo came since; then it is a dead end. *)
val next_number : t -> string -> (outcome, string) result

val next_relation : t -> relation -> (outcome, string) result

(** lists the hunt's smaller groups; [set] uses the latest listing until the hunt changes *)
val results : t -> all:bool -> (row list, string) result

(** writes the number, waits ([settle]), then reads the address back: the game putting back the
    bytes the write replaced, or anything else there, is a second line and [held = false]. The
    write can be undone either way. *)
val set : t -> row:int -> string -> (set_done, string) result

(** writes back the exact bytes the newest write replaced *)
val undo : t -> (string * written, string) result

(** the writes [undo] can still reverse: addr, vtype, previous, new; newest first *)
val writes : t -> (int64 * string * Helper_api.value * Helper_api.value) list

(** the hunt as it stands, or [None] when there is none: the counts, tags, groups and the notes
    the groups imply ([Showing_wide], [Untracked]); [ms], [bytes] and [unreadable] are 0 *)
val current : t -> outcome option
