(** moth's background service: keeps the helper and the hunt alive between commands. It serves
    the client protocol on [home]/core.sock, one request per connection and one request at a
    time, in arrival order, and appends one line per recorded request to [home]/events.jsonl.
    A [watch] connection stays open: up to 16 watchers are sent a line as each recorded request
    starts and its event once it ends, and one that can't take a line at once is let go. A read
    sent with [record: false] leaves no line and tells no watcher (see docs/dev.md). *)

(** starts a helper inside the named bottle *)
type start_helper = bottle:string -> (Helper_api.helper, string) result

(** serves until a "stop" request, SIGTERM or SIGINT; on the way out it closes the helper
    connection and removes the socket. Raises [Failure] with the message to show when another
    daemon already serves [home] or the socket path is too long.

    MOTH_BUDGET_MB is read once, here: a whole number from 1 to 65536 is the memory budget, in
    MiB, that every hunt's [find] sends with its scan; anything else is ignored, with a line in
    the log (unset or empty, quietly). MOTH_SNAPSHOT_MB likewise: a whole number from 1 to
    1048576 is the most memory, in MiB, a [snapshot] may take, sent with each one. *)
val run : home:string -> start_helper:start_helper -> unit

(** the real helper under CrossOver's wine, its output appended to [home]/helper.log *)
val default_start_helper : home:string -> start_helper

(** the arguments and options a command takes; the same messages the daemon replies with *)
val check_usage : cmd:string -> args:string list -> opts:(string * string) list -> (unit, string) result

(** an event from events.jsonl as [moth log] prints it; None when the object is not an event *)
val event_line : Yojson.Safe.t -> string option

(** [home]/core.sock, or the error to show when the path is too long for a unix socket *)
val socket_path : home:string -> (string, string) result

(** creates [home] (mode 0700) when it is missing; raises [Failure] with the message to show when
    it can't *)
val make_home : string -> unit

(** removes [home]/core.sock unless a daemon holds [home]'s lock (one is running or starting);
    false when one does *)
val clear_stale_socket : home:string -> bool
