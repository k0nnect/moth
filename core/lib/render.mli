(** What the user reads. *)

(** the counts line, then a line saying how much memory could not be read (when some could not),
    then one line per note in a fixed order; a dead end is one line alone, saying what to try
    instead: after a relation step, to say again how it moved or type the number; after a
    number, to try another or say how it moved when [Session.can_compare], else to start over
    with a snapshot. Under [Snapshot_taken] a line saying how much was taken and what to do next
    is in the counts line's place. *)
val outcome : Session.outcome -> string

(** bytes as the user reads them, in 1024s: ["4 kb"], ["12 mb"], ["1.2 gb"] (never under 1 kb);
    [Session.size] *)
val size : int -> string

(** the counts line alone: the total shown (the wide tier's under [Showing_wide]) and its tags *)
val counts : Session.outcome -> string

(** status's line, in the counts line's place, for a hunt from a snapshot that no step has
    narrowed yet ([Session.blind]) *)
val no_step_yet : string

(** one line per row: number, value, address, tag; rows near a value also have their distance
    from it, as ["+16"], ["-8"] or ["0"], before the tag *)
val rows : Session.row list -> string

(** a page of results: its rows, then, when rows remain, the line saying which rows these are
    and of how many (with thousands separators) and the command for the next page, which
    repeats [--all] and [--near <target>] when given *)
val listing : ?near:string -> all:bool -> Session.listing -> string

(** one line per kept value: name, value, address, tag; a value that reads as it did before its
    newest write ([Session.kept]'s [put_back]) then says what it was set to and that it is back
    to what it was before *)
val kept : Session.kept list -> string

(** the words [kept] prints after the tag for one value: that it was set and is back to what it
    was before, or [""] *)
val kept_note : Session.kept -> string

(** [1234567] is ["1,234,567"] *)
val commas : int -> string
