type t = Int of int64 | Dec of { text : string; value : float; decimals : int }

(** parse what the user typed; trims, drops [,] and [_], optional sign *)
val parse : string -> (t, string) result

val to_float : t -> float
