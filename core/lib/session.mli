(** The hunt: what to ask the helper for each number or change the user reports, and what
    came of it. The helper holds the candidates; this keeps the groups (one per interpretation
    from [Interp.plan], in plan order), the last number typed, the last results listing, the
    writes that can still be undone and the values kept by name. Losing the game or the helper
    ends the hunt and drops the writes and the kept values; a lost group ends only the hunt. *)

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

(** one result: its number in the listing, where it is, the interpretation and how it reads;
    [away], in a listing near a value, is its signed distance from that value in bytes *)
type row = { n : int; addr : int64; interp_id : string; tag : string; shown : string; away : int option }

(** a page of a listing: its rows, how many rows the whole listing has, and the first row asked for *)
type listing = { rows : row list; total : int; from : int }

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

(** narrows to the number typed. A float reading in a group with stored values must also have
    moved when the ranges this number and the last number typed give it overlap only in part, or
    not at all; when one range holds the other (5 and 5.3), it need not. With no exact match it
    falls back on the direction the number moved from the last number typed, unless a relation
    step, a fallback, a set or an undo came since; then it is a dead end. A fallback is a relation
    step: after it there is no last number typed. *)
val next_number : t -> string -> (outcome, string) result

val next_relation : t -> relation -> (outcome, string) result

(** 4096: how far a listing near a value looks, in bytes, each way *)
val reach : int

(** rows [from] to [from + limit - 1] (1-based) of a listing of the groups shown: the tracked
    ones with matches in main, and in wide too under [all] or when only wide is left.

    Without [near], the groups go fewest matches first, ties in plan order, each in address
    order, numbered from 1 across that order; [total] is their counts summed. With [near] (a kept
    value's name, or a [0x] address), the rows are every position within [reach] bytes of that
    address, its own included, nearest first, ties in plan order. Either way a [from] past
    [total] (when there are any) is an error. The page's rows are the ones [set] and [keep_row]
    take, until the hunt changes or another listing replaces them. *)
val results : t -> all:bool -> from:int -> limit:int -> near:string option -> (listing, string) result

(** writes the number, waits ([settle]), then reads the address back: the game putting back the
    bytes the write replaced, or anything else there, is a second line and [held = false]. The
    write can be undone either way. *)
val set : t -> row:int -> string -> (set_done, string) result

(** writes back the exact bytes the newest write replaced *)
val undo : t -> (string * written, string) result

(** a kept value: its name, address and tag, and how it reads now (["?"] when it can't be read) *)
type kept = { name : string; kaddr : int64; ktag : string; kshown : string }

(** keeps a row of the latest listing by name, or as the first of [v1], [v2], ... not in use. A
    name is a lowercase letter and up to 23 lowercase letters, digits, [_] or [-]; at most 64
    values are kept. Kept values outlast the hunt. *)
val keep_row : t -> row:int -> name:string option -> (string, string) result

(** keeps the address as the first interpretation of the number, in plan order, that reads as it
    there *)
val keep_at : t -> addr:int64 -> string -> name:string option -> (string, string) result

(** the kept values, read now, in the order kept *)
val kept : t -> (kept list, string) result

val forget : t -> string -> (string, string) result

(** [set] for a kept value, named in its texts and in [undo]'s *)
val set_kept : t -> name:string -> string -> (set_done, string) result

(** the writes [undo] can still reverse: addr, vtype, previous, new; newest first *)
val writes : t -> (int64 * string * Helper_api.value * Helper_api.value) list

(** the hunt as it stands, or [None] when there is none: the counts, tags, groups and the notes
    the groups imply ([Showing_wide], [Untracked]); [ms], [bytes] and [unreadable] are 0 *)
val current : t -> outcome option
