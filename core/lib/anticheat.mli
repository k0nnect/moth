(** the display name of the first anti-cheat found among the running process names and the
    loaded module names, compared case-insensitively *)
val check : processes:string list -> modules:string list -> string option
