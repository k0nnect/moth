(** Starting the helper inside a bottle and talking to it over TCP. *)

(** starts the helper so that it connects to [127.0.0.1:port] and says hello with [token];
    [log] is where its output goes. It must not wait for the helper to finish. *)
type launcher = port:int -> token:string -> log:string -> (unit, string) result

(** runs [helper_exe] under CrossOver's wine in [bottle], stdout and stderr appended to the log *)
val wine_launcher : bottle:string -> helper_exe:string -> launcher

(** [hello_timeout] (default 5 seconds) is how long one connection may take to send its hello line
    (at most 4096 bytes).

    listens on an ephemeral loopback port, calls [launch], and waits (default 60 seconds) for a
    connection whose hello carries the right token; a connection with another token is dropped and
    waiting goes on. Requests time out after 300 seconds for scan and narrow, 30 otherwise. A
    closed, broken, timed-out or out-of-step connection gives the error code [helper_lost] from
    then on. *)
val start : launch:launcher -> label:string -> log:string -> ?timeout:float -> ?hello_timeout:float -> unit -> (Helper_api.helper, string) result

(** MOTH_HELPER (made absolute), else moth-helper.exe in the directory of the moth executable *)
val helper_exe : unit -> (string, string) result
