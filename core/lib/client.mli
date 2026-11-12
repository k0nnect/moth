(** Talking to moth's background service, and reading the [moth] command line. *)

(** MOTH_HOME (made absolute), or ~/Library/Application Support/moth *)
val home : unit -> string

(** the error [call ~autostart:false] gives when no daemon answers *)
val not_running : string

(** sends one request and waits for its reply: the result and the rendered text, or the error.
    When no daemon answers and [autostart] (default true) is set, it removes a stale core.sock,
    starts one with [spawn_daemon] and tries again for up to 5 seconds. The default
    [spawn_daemon] runs [Sys.executable_name daemon] in its own session, with no terminal, stdin
    from /dev/null and output appended to [home]/core.log. *)
val call :
  ?autostart:bool ->
  ?spawn_daemon:(home:string -> unit) ->
  home:string ->
  actor:string ->
  cmd:string ->
  args:string list ->
  opts:(string * string) list ->
  unit ->
  (Yojson.Safe.t * string, string) result

type invocation = { cmd : string; args : string list; opts : (string * string) list; json : bool }

(** [moth <cmd> [args] [--bottle <b>] [--all] [--json] [-n <n>] [--from <row>] [--near <v>]]
    without the program name. Options may come anywhere; an argument like [-5] or [-.5] is a
    number, not an option. *)
val parse_args : string list -> (invocation, string) result
