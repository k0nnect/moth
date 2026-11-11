(** What the user reads. *)

(** the counts line, then a line saying how much memory could not be read (when some could not),
    then one line per note in a fixed order; a dead end is one line alone *)
val outcome : Session.outcome -> string

(** bytes as the user reads them, in 1024s: ["4 kb"], ["12 mb"], ["1.2 gb"] (never under 1 kb) *)
val size : int -> string

(** the counts line alone: the total shown (the wide tier's under [Showing_wide]) and its tags *)
val counts : Session.outcome -> string

(** one line per row: number, value, address, tag *)
val rows : Session.row list -> string

(** [1234567] is ["1,234,567"] *)
val commas : int -> string
