(** moth's background service: keeps the helper and the hunt alive between commands. It serves
    the client protocol on [home]/core.sock, one request per connection and one request at a
    time, in arrival order, and appends one line per request to [home]/events.jsonl. *)

(** starts a helper inside the named bottle *)
type start_helper = bottle:string -> (Helper_api.helper, string) result

(** serves until a "stop" request, SIGTERM or SIGINT; on the way out it closes the helper
    connection and removes the socket. Raises [Failure] with the message to show when another
    daemon already serves [home] or the socket path is too long. *)
val run : home:string -> start_helper:start_helper -> unit

(** the real helper under CrossOver's wine, its output appended to [home]/helper.log *)
val default_start_helper : home:string -> start_helper

(** the arguments and options a command takes; the same messages the daemon replies with *)
val check_usage : cmd:string -> args:string list -> opts:(string * string) list -> (unit, string) result

(** [home]/core.sock, or the error to show when the path is too long for a unix socket *)
val socket_path : home:string -> (string, string) result

(** creates [home] (mode 0700) when it is missing; raises [Failure] with the message to show when
    it can't *)
val make_home : string -> unit

(** removes [home]/core.sock unless a daemon holds [home]'s lock (one is running or starting);
    false when one does *)
val clear_stale_socket : home:string -> bool
