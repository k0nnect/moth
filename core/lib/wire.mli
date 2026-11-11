(** The helper protocol: newline-delimited JSON, as in the plan's "Protocols" table. Integer
    values are decimal strings, float values JSON numbers or, when not finite, the strings
    ["nan"], ["inf"] and ["-inf"]; addresses lowercase [0x] hex strings read and printed as
    unsigned 64-bit; a value's raw bytes ([bits], [prev_bits]) a lowercase [0x] hex string of two
    digits per byte of its type.

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

(** the bytes a value of the type takes: 1, 2, 4 or 8 *)
val width : string -> int

(** a value's raw bytes (the low [width vtype] bytes of the int64) as [0x] and two digits per byte *)
val bits_to_json : vtype:string -> int64 -> Yojson.Safe.t

(** [0x] and up to two hex digits per byte of the type *)
val bits_of_json : vtype:string -> Yojson.Safe.t -> int64

(** the raw bytes the helper writes for a value of the type: a whole number's low bytes, a float's
    IEEE bits (an [f32] rounded to single precision first) *)
val bits_of_value : vtype:string -> Helper_api.value -> int64
