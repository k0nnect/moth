(** What the user reads. *)

(** the counts line, then one line per note in a fixed order; a dead end is one line alone *)
val outcome : Session.outcome -> string

(** one line per row: number, value, address, tag *)
val rows : Session.row list -> string

(** [1234567] is ["1,234,567"] *)
val commas : int -> string
