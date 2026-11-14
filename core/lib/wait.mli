(** What the person did in moth, bundled into one note for the agent. No sockets, files or clock:
    the caller gives every call the time. *)

type t

(** [cursor]: the newest event already dealt with; [quiet]: whether the agent is not to be told *)
val start : cursor:int -> quiet:bool -> t

(** the text of a session's cursor file: the seq, and [" quiet"] when moth was quiet at it
    ([123 quiet]) *)
val cursor_text : int -> bool -> string

(** the seq and the quiet state in a cursor file's (trimmed) text; None when it is neither form *)
val parse_cursor : string -> (int * bool) option

(** a feed line (a [busy] line or an event), seen at [now]. An event counts when its [seq] is
    above the cursor, its actor is not [agent], its [act] is true and its command is not [quiet],
    and moth is not quiet. A [quiet] event, from anyone, sets the quiet state when it succeeded;
    turning quiet on drops what is held, and turning it off replays nothing. *)
val line : t -> now:float -> Yojson.Safe.t -> unit

type verdict = Keep_waiting | Tell of { notes : string; cursor : int }

(** called after every line and every idle tick. [Tell] when an event is held, no [busy] line is
    waiting for its event, and 1.5 seconds have passed since the last line or 10 since the first
    held event. Telling empties what is held. *)
val check : t -> now:float -> verdict

(** the newest seq taken in, told or not *)
val cursor : t -> int

(** whether the agent is not being told *)
val quiet : t -> bool

(** the feed was dropped: no [busy] line seen before it is waited for any more *)
val dropped : t -> unit
