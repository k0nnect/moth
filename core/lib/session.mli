(** The hunt: what to ask the helper for each number or change the user reports, and what
    came of it. The helper holds the candidates; this keeps the groups (one per interpretation
    from [Interp.plan], or from [Interp.blind_plan] for a hunt from a snapshot, in plan order),
    the last number typed, the last results listing, the writes that can still be undone and the
    values kept by name. One hunt at a time: a [find] or a [snapshot] replaces the hunt there
    was. Losing the game or the helper ends the hunt and drops the writes and the kept values; a
    lost group ends only the hunt. The helper running out of memory part-way through a step, or
    no longer holding the values a step compares with, leaves the hunt as it was. *)

type t
type relation = Down | Up | Same | Changed

(** [Snapshot_taken]: the hunt is a snapshot just taken, every aligned place a candidate *)
type note = Fell_back of relation | Showing_wide | Untracked | Lost of int | Snapshot_taken

(** [main] and [wide]: matches in the tracked groups of each tier. [by_tag]: the tracked groups
    being shown (wide ones only under [Showing_wide]) summed per [Interp.describe] tag, in plan
    order, zero counts left out. A dead end reports the hunt as it stands, unchanged.
    [by_relation]: the step was by how the value moved ([next_relation]); a number's fallback is
    not one. [ms], [bytes] and [unreadable]: the helper's time, bytes read and bytes it could not
    read (scans only), summed over the calls the step made. [groups]: the hunt's groups in plan
    order. *)
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
  groups : (Helper_api.group_info * Interp.tier) list;
}

(** one result: its number in the listing, where it is, the interpretation and how it reads
    (["?"] when it can't be read); [away], in a listing near a value, is its signed distance from
    that value in bytes *)
type row = { n : int; addr : int64; interp_id : string; tag : string; shown : string; away : int option }

(** a page of a listing: its rows, how many rows the whole listing has, and the first row asked for *)
type listing = { rows : row list; total : int; from : int }

(** one write as it was made: where, as which type, the raw bytes it replaced and the ones it put
    there (see [Wire.bits_of_value]) *)
type written = { where : int64; wtype : string; bits_before : int64; bits_after : int64 }

(** what to tell the user, the write, and whether the address still held it a moment later:
    [Some true] it read back as written, [Some false] it did not, [None] it could not be read
    back (the text's second line says why) *)
type set_done = { text : string; w : written; held : bool option }

(** [budget_mb]: sent on a find's scan and on a snapshot, none by default. [snapshot_mb]: the
    most memory a snapshot may take, sent on each, none by default (the helper's own limit).
    [settle]: the wait between a write and reading it back, a quarter of a second by default. *)
val create : ?budget_mb:int -> ?snapshot_mb:int -> ?settle:(unit -> unit) -> Helper_api.helper -> t

(** starts a new hunt. A scan that fails ends the hunt there was. *)
val find : t -> string -> (outcome, string) result

(** starts a new hunt from a snapshot of the game's memory: every aligned place of each of
    [Interp.blind_plan]'s groups, with no number typed, so no baseline. The outcome carries
    [Snapshot_taken] and the bytes read. Until a step applies there is nothing to list. When the
    helper's size check refuses, the hunt there was stays and the error gives both sizes; any
    other failure ends it, as a failed [find] does. *)
val snapshot : t -> (outcome, string) result

(** bytes as the user reads them, in 1024s: ["4 kb"], ["12 mb"], ["1.2 gb"] (never under 1 kb);
    [Render.size] is this *)
val size : int -> string

(** narrows to the number typed. A float reading in a group with stored values must also have
    moved when the ranges this number and the last number typed give it overlap only in part, or
    not at all; when one range holds the other (5 and 5.3), it need not. With no exact match it
    falls back on the direction the number moved from the last number typed, unless a relation
    step, a fallback, a set or an undo came since, or none was typed (a hunt from a snapshot);
    then it is a dead end. A fallback is a relation step: after it there is no last number
    typed. *)
val next_number : t -> string -> (outcome, string) result

val next_relation : t -> relation -> (outcome, string) result

(** a relation step can be applied to the outcome's hunt: one of its groups is tracked, has
    matches, and has stored values (its own or the snapshot's) to compare with, which
    [next_relation] asks before it sends anything *)
val can_compare : outcome -> bool

(** the hunt is from a snapshot and no step has narrowed it yet: every aligned place is still a
    candidate, and there is nothing to list *)
val blind : t -> bool

(** 4096: how far a listing near a value looks, in bytes, each way *)
val reach : int

(** rows [from] to [from + limit - 1] (1-based) of a listing of the groups shown: the tracked
    ones with matches in main, and in wide too under [all] or when only wide is left. A hunt from
    a snapshot that no step has narrowed yet has nothing to list, and says to step first.

    Without [near], the groups go fewest matches first, ties in plan order, each in address
    order, numbered from 1 across that order; [total] is their counts summed. With [near] (a kept
    value's name, or a [0x] address), the rows are every position within [reach] bytes of that
    address, its own included, nearest first, ties in plan order. Either way rows are numbered by
    position: a position whose value can't be read is a row in its place, shown as ["?"]. A
    [from] past [total] (when there are any) is an error. When [remember] (the default), the
    page's rows are the ones [set] and [keep_row] without [at] take, until the hunt changes or
    another remembered listing replaces them; a listing not remembered changes nothing. *)
val results : ?remember:bool -> t -> all:bool -> from:int -> limit:int -> near:string option -> (listing, string) result

(** writes the number, waits ([settle]), then reads the address back: the game putting back the
    bytes the write replaced, or anything else there, is a second line and [held = Some false]. A
    read-back that can't happen is [held = None] and a second line saying why; when it lost the
    game or the helper, the hunt, the kept values and the undo list end there, as they would in
    any step. The write landed, so it is the result in every case, and it can be undone unless
    the undo list ended.

    Without [at], [row] is the row of that number in the last remembered listing. With [at], it
    is found afresh in the hunt, numbered as [results] numbers rows without [near] ([all] as
    given, false by default), and the remembered listing is neither used nor changed; that row
    must be at [at] and, when [kind] is given, be the interpretation with that id, or nothing is
    written and the error says the list changed. *)
val set : ?at:int64 -> ?kind:string -> ?all:bool -> t -> row:int -> string -> (set_done, string) result

(** writes back the exact bytes the newest write replaced *)
val undo : t -> (string * written, string) result

(** a kept value: its name, address and tag, and how it reads now (["?"] when it can't be read).
    [set_to]: what its newest write wrote, shown as the kept value shows numbers. Its newest
    write is the newest that [undo] can still reverse at its address and as its value type,
    however it was made: by row or by name, before or after it was kept. [put_back]: it reads as
    it did before that write (the bytes there now are the ones the write replaced, not the ones
    it wrote); never when it can't be read. *)
type kept = { name : string; kaddr : int64; ktag : string; kshown : string; set_to : string option; put_back : bool }

(** keeps a row by name, or as the first of [v1], [v2], ... not in use, and says how it reads now
    (as the listing showed it when it can't be read). A name is a lowercase letter and up to 23
    lowercase letters, digits, [_] or [-]; at most 64 values are kept. Kept values outlast the
    hunt. The row is found as [set] finds it: in the last remembered listing, or with [at] (and
    [kind], [all]) afresh, and nothing is kept when it is not there. *)
val keep_row : ?at:int64 -> ?kind:string -> ?all:bool -> t -> row:int -> name:string option -> (string, string) result

(** keeps the address as the first interpretation of the number, in plan order, that reads as it
    there *)
val keep_at : t -> addr:int64 -> string -> name:string option -> (string, string) result

(** the kept values, read now, in the order kept *)
val kept : t -> (kept list, string) result

val forget : t -> string -> (string, string) result

(** [set] for a kept value, named in its texts and in [undo]'s. A name that is not kept but has
    the form of an address gets an answer saying to keep it first. *)
val set_kept : t -> name:string -> string -> (set_done, string) result

(** the writes [undo] can still reverse: addr, vtype, previous, new; newest first *)
val writes : t -> (int64 * string * Helper_api.value * Helper_api.value) list

(** the hunt as it stands, or [None] when there is none: the counts, tags, groups and the notes
    the groups imply ([Showing_wide], [Untracked]); [ms], [bytes] and [unreadable] are 0 *)
val current : t -> outcome option
