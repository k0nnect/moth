(** What the user reads. *)

(** the counts line, then a line saying how much memory could not be read (when some could not),
    then one line per note in a fixed order; a dead end is one line alone. Under
    [Snapshot_taken] a line saying how much was taken and what to do next is in the counts
    line's place. *)
val outcome : Session.outcome -> string

(** bytes as the user reads them, in 1024s: ["4 kb"], ["12 mb"], ["1.2 gb"] (never under 1 kb);
    [Session.size] *)
val size : int -> string

(** the counts line alone: the total shown (the wide tier's under [Showing_wide]) and its tags *)
val counts : Session.outcome -> string

(** one line per row: number, value, address, tag; rows near a value also have their distance
    from it, as ["+16"], ["-8"] or ["0"], before the tag *)
val rows : Session.row list -> string

(** a page of results: its rows, then, when rows remain, the line saying which rows these are
    and of how many (with thousands separators) and the command for the next page, which
    repeats [--all] and [--near <target>] when given *)
val listing : ?near:string -> all:bool -> Session.listing -> string

(** one line per kept value: name, value, address, tag; a value the game put back after a write
    by name ([Session.kept]'s [put_back]) then says what it was set to and that the game put it
    back *)
val kept : Session.kept list -> string

(** [1234567] is ["1,234,567"] *)
val commas : int -> string
