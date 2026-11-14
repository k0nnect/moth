(** Talking to moth's background service, and reading the [moth] command line. *)

(** MOTH_HOME (made absolute), or ~/Library/Application Support/moth *)
val home : unit -> string

(** the error [call ~autostart:false] gives when no daemon answers *)
val not_running : string

(** sends one request and waits for its reply: the result and the rendered text, or the error.
    [record:false] asks for a read that leaves no trace (see [Daemon]). [timeout] is the most
    seconds to wait for the reply (none by default); a service that stays silent is then lost
    contact.
    When no daemon answers and [autostart] (default true) is set, it removes a stale core.sock,
    starts one with [spawn_daemon] and tries again for up to 5 seconds. The default
    [spawn_daemon] runs [Sys.executable_name daemon] in its own session, with no terminal, stdin
    from /dev/null and output appended to [home]/core.log. *)
val call :
  ?autostart:bool ->
  ?record:bool ->
  ?timeout:float ->
  ?spawn_daemon:(home:string -> unit) ->
  home:string ->
  actor:string ->
  cmd:string ->
  args:string list ->
  opts:(string * string) list ->
  unit ->
  (Yojson.Safe.t * string, string) result

(** sends [watch] (with [after], when given), then calls the function with each feed line, the
    whole object, until it returns false or the daemon closes the connection; then [Ok ()]. An
    error reply (such as [too many watchers]) is the error. [timeout] is the most seconds to wait for the reply (none by default), after which it is lost contact. [started] is called once with the [seq]
    in the reply, before any line. [idle] is called whenever half a second passes with no line;
    when it returns false the watch ends with [Ok ()]. It never starts a daemon: with none
    answering it is [Error not_running]. *)
val watch :
  home:string -> ?after:int -> ?timeout:float -> ?started:(int -> unit) -> ?idle:(unit -> bool) -> (Yojson.Safe.t -> bool) -> (unit, string) result

(** who is speaking, from the value of MOTH_ACTOR: [agent] is the agent; [user], empty or unset is
    the user; anything else is the error [MOTH_ACTOR must be user or agent] *)
val actor_of_env : string option -> (string, string) result

type invocation = { cmd : string; args : string list; opts : (string * string) list; json : bool }

(** [moth <cmd> [args] [--bottle <b>] [--all] [--json] [-n <n>] [--from <row>] [--near <v>]
    [--after <seq>] [--at <0xaddr>] [--kind <interp_id>] [--session <dir>]] without the program name. Options may
    come anywhere; an argument like [-5] or [-.5] is a number, not an option.

    [--at] goes with a row write, [set <#n> <number>] or [keep <#n> [name]]: row n is then found
    afresh in the hunt, numbered as [results] numbers it without [--near], and must be at that
    address. [--kind] (an [interp_id] as [results --json] gives it) also pins the row's
    interpretation, and [--all] counts the rarer forms when numbering, as [results --all] does;
    on [set] and [keep] both go only with [--at]. Without [--at], row n is the one the last
    recorded [results] listed. *)
val parse_args : string list -> (invocation, string) result
