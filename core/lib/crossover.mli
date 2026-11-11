(** MOTH_CROSSOVER, then ~/Applications/CrossOver.app, then /Applications/CrossOver.app. A set
    MOTH_CROSSOVER is the only place looked at. *)
val find_app : unit -> (string, string) result

(** MOTH_BOTTLES, or ~/Library/Application Support/CrossOver/Bottles *)
val bottles_dir : unit -> string

(** sorted names of the directories in [bottles_dir] that contain a cxbottle.conf *)
val list_bottles : unit -> string list

val check_bottle : string -> (unit, string) result

(** wine's command line: [wine --bottle b --workdir (dirname exe) exe args...] *)
val wine_argv : app:string -> bottle:string -> exe:string -> string list -> string array
