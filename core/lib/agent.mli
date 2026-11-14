(** Starting Claude Code on a moth session: what it is told, and the files it is started with. *)

(** what the agent is told about moth, appended to its own system prompt *)
val instructions : string

(** what moth says to it first, so its first turn ends and the hook is armed *)
val first_prompt : string

(** the settings for one session: env, the one approval, the Stop hook. [moth] is the absolute
    path of the running [moth], [path] the PATH it was started with, [dir] the session folder. *)
val settings : home:string -> moth:string -> path:string -> dir:string -> Yojson.Safe.t

(** makes [home]/agent/<pid> (0700) with settings.json and instructions.md (0600), makes
    [home]/agent/work, and removes sibling session folders whose process is not alive (a folder
    named for a process id; nothing else under agent/ is touched). Returns the session folder. *)
val prepare : home:string -> moth:string -> path:string -> pid:int -> (string, string) result

(** the program and arguments to run *)
val command : claude:string -> dir:string -> string array

(** removes a session folder made by [prepare] *)
val remove_session : string -> unit
