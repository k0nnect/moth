type tier = Main | Wide
type encoding = Plain | Scaled of int | Fraction
type t = { id : string; vtype : string; align : int; exclude_align : int; tier : tier; encoding : encoding }

(** priority order; also the order sent to the helper *)
val plan : Value.t -> t list

(** [None] when the number can't be held: too big, a decimal for a whole number, or more decimal
    places than a scaled form keeps (99.25 in tenths) *)
val pred_for : t -> Value.t -> Helper_api.pred option

(** what to write for the number, or why it can't be written there *)
val encode : t -> Value.t -> (Helper_api.value, string) result

(** what to show the user *)
val decode : t -> Helper_api.value -> string

val describe : t -> string
