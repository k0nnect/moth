(** The helper protocol: newline-delimited JSON, as in the plan's "Protocols" table. Integer
    values are decimal strings, float values JSON numbers, addresses lowercase [0x] hex strings
    read and printed as unsigned 64-bit.

    The decoders take the value of one reply field (for example the [groups] array, not the whole
    reply) and raise [Failure] when it does not have the shape the table gives. *)

(** one request line, without the newline: [{"id":..,"op":..}] followed by the fields in order *)
val request : id:int -> op:string -> (string * Yojson.Safe.t) list -> string

val group_spec_to_json : Helper_api.group_spec -> Yojson.Safe.t
val pred_to_json : Helper_api.pred -> Yojson.Safe.t
val addr_to_json : int64 -> Yojson.Safe.t

(** the id and the whole reply object of an ok reply, or the reply's error. A line that is not a
    reply gives the error code [bad_reply]. *)
val parse_reply : string -> (int * Yojson.Safe.t, Helper_api.err) result

val group_infos : Yojson.Safe.t -> Helper_api.group_info list
val stats : Yojson.Safe.t -> Helper_api.stats
val items : vtype:string -> Yojson.Safe.t -> Helper_api.item list
val value_to_json : Helper_api.value -> Yojson.Safe.t
val value_of_json : vtype:string -> Yojson.Safe.t -> Helper_api.value
