type tier = Main | Wide
type encoding = Plain | Scaled of int | Fraction
type t = { id : string; vtype : string; align : int; exclude_align : int; tier : tier; encoding : encoding }

(** priority order; also the order sent to the helper *)
val plan : Value.t -> t list

val pred_for : t -> Value.t -> Helper_api.pred option
val encode : t -> Value.t -> (Helper_api.value, string) result

(** what to show the user *)
val decode : t -> Helper_api.value -> string

val describe : t -> string
