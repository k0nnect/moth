let ( let* ) = Result.bind
let sprintf = Printf.sprintf

let home () =
  match Sys.getenv_opt "MOTH_HOME" with
  | Some h when h <> "" -> if Filename.is_relative h then Filename.concat (Sys.getcwd ()) h else h
  | _ ->
      let user = match Sys.getenv_opt "HOME" with Some h when h <> "" -> h | _ -> "/" in
      Filename.concat user "Library/Application Support/moth"

let not_running = "moth isn't running"
let core_log home = Filename.concat home "core.log"

let connect path =
  let s = Unix.socket ~cloexec:true Unix.PF_UNIX Unix.SOCK_STREAM 0 in
  match Unix.connect s (Unix.ADDR_UNIX path) with
  | () -> Some s
  | exception Unix.Unix_error _ ->
      Unix.close s;
      None

(* [moth daemon] cut loose from the caller: a new session, so no controlling terminal; a
   grandchild, so it can never take one back and the caller isn't its parent; stdin from
   /dev/null and output appended to core.log, so it never holds the caller's terminal or pipes *)
let spawn_daemon ~home =
  match Unix.fork () with
  | 0 ->
      (try
         ignore (Unix.setsid ());
         if Unix.fork () = 0 then begin
           let null = Unix.openfile "/dev/null" [ Unix.O_RDONLY ] 0 in
           let out = Unix.openfile (core_log home) [ Unix.O_WRONLY; Unix.O_CREAT; Unix.O_APPEND ] 0o600 in
           Unix.dup2 null Unix.stdin;
           Unix.dup2 out Unix.stdout;
           Unix.dup2 out Unix.stderr;
           Unix.close null;
           Unix.close out;
           Unix.putenv "MOTH_HOME" home;
           Unix.execv Sys.executable_name [| Sys.executable_name; "daemon" |]
         end
       with e -> prerr_endline ("couldn't start moth daemon: " ^ Printexc.to_string e));
      Unix._exit 0
  | pid ->
      let rec wait () = match Unix.waitpid [] pid with _ -> () | exception Unix.Unix_error (Unix.EINTR, _, _) -> wait () in
      wait ()

let bad_reply = "moth's background service sent something that isn't a reply"

let parse_reply line =
  let open Yojson.Safe.Util in
  match Yojson.Safe.from_string line with
  | `Assoc _ as r -> (
      match member "ok" r with
      | `Bool true -> Ok (member "result" r, match member "text" r with `String t -> t | _ -> "")
      | _ -> Error (match member "error" r with `String e -> e | _ -> bad_reply))
  | _ | (exception Yojson.Json_error _) -> Error bad_reply

(* sends the request and reads its reply, which [k] takes with the connection still open *)
let converse ~home fd request k =
  let ic = Unix.in_channel_of_descr fd in
  Fun.protect
    ~finally:(fun () -> close_in_noerr ic)
    (fun () ->
      let line = Yojson.Safe.to_string request ^ "\n" in
      match
        ignore (Unix.write_substring fd line 0 (String.length line));
        input_line ic
      with
      | reply -> k ic (parse_reply reply)
      | exception (End_of_file | Sys_error _ | Unix.Unix_error _) ->
          Error (sprintf "lost contact with moth's background service. see %s" (core_log home)))

let exchange ~home fd request = converse ~home fd request (fun _ reply -> reply)

(* removes a stale socket and starts a daemon, then waits up to 5 seconds for it to answer. While
   a daemon holds home's lock (it is starting or stopping) the socket is left alone and no second
   daemon is started. *)
let start_and_connect ~spawn_daemon ~home path =
  let deadline = Unix.gettimeofday () +. 5. in
  let rec retry spawned =
    match connect path with
    | Some fd -> Ok fd
    | None when Unix.gettimeofday () >= deadline -> Error (sprintf "couldn't start moth's background service. see %s" (core_log home))
    | None ->
        let spawned = spawned || (Daemon.clear_stale_socket ~home && (spawn_daemon ~home; true)) in
        Unix.sleepf 0.05;
        retry spawned
  in
  match Daemon.make_home home with exception Failure m -> Error m | () -> retry false

let call ?(autostart = true) ?(spawn_daemon = spawn_daemon) ~home ~actor ~cmd ~args ~opts () =
  Sys.set_signal Sys.sigpipe Sys.Signal_ignore;
  let* path = Daemon.socket_path ~home in
  let* fd =
    match connect path with
    | Some fd -> Ok fd
    | None when not autostart -> Error not_running
    | None -> start_and_connect ~spawn_daemon ~home path
  in
  let request =
    `Assoc
      [
        ("id", `Int 1);
        ("actor", `String actor);
        ("cmd", `String cmd);
        ("args", `List (List.map (fun a -> `String a) args));
        ("opts", `Assoc (List.map (fun (k, v) -> (k, `String v)) opts));
      ]
  in
  exchange ~home fd request

(* every feed line after watch's reply, until [f] says stop or the daemon closes the connection.
   A line cut short (the daemon let go of this watcher mid-line) is not json, and ends it too. *)
let watch ~home ?after ?started ?idle f =
  Sys.set_signal Sys.sigpipe Sys.Signal_ignore;
  let* path = Daemon.socket_path ~home in
  match connect path with
  | None -> Error not_running
  | Some fd ->
      Fun.protect ~finally:(fun () -> try Unix.close fd with Unix.Unix_error _ -> ()) @@ fun () ->
      let opts = match after with Some k -> [ ("after", `String (string_of_int k)) ] | None -> [] in
      let request = `Assoc [ ("id", `Int 1); ("cmd", `String "watch"); ("args", `List []); ("opts", `Assoc opts) ] in
      let lost = Error (sprintf "lost contact with moth's background service. see %s" (core_log home)) in
      (* lines are read by hand, so that [idle] gets its say whenever half a second passes with
         nothing to read: [`Line l], [`Idle] or [`Eof] *)
      let pending = Buffer.create 4096 and chunk = Bytes.create 4096 in
      let rec next ~idling =
        let held = Buffer.contents pending in
        match String.index_opt held '\n' with
        | Some j ->
            Buffer.clear pending;
            Buffer.add_string pending (String.sub held (j + 1) (String.length held - j - 1));
            `Line (String.sub held 0 j)
        | None -> (
            match Unix.select [ fd ] [] [] 0.5 with
            | [], _, _ -> if idling then `Idle else next ~idling
            | _ -> (
                match Unix.read fd chunk 0 (Bytes.length chunk) with
                | 0 -> `Eof
                | n ->
                    Buffer.add_subbytes pending chunk 0 n;
                    next ~idling
                | exception Unix.Unix_error (Unix.EINTR, _, _) -> next ~idling
                | exception Unix.Unix_error _ -> `Eof)
            | exception Unix.Unix_error (Unix.EINTR, _, _) -> next ~idling)
      in
      let line = Yojson.Safe.to_string request ^ "\n" in
      let sent = try ignore (Unix.write_substring fd line 0 (String.length line)); true with Unix.Unix_error _ -> false in
      if not sent then lost
      else
        (* the reply is waited for as long as converse waits: no idle turns before it *)
        match next ~idling:false with
        | `Eof -> lost
        | `Idle -> lost
        | `Line reply ->
            let* result, _ = parse_reply reply in
            (match (started, result) with
            | Some k, `Assoc fields -> ( match List.assoc_opt "seq" fields with Some (`Int n) -> k n | _ -> ())
            | _ -> ());
            let rec follow () =
              match next ~idling:true with
              | `Eof -> Ok ()
              | `Idle -> if (match idle with Some i -> i () | None -> true) then follow () else Ok ()
              | `Line l -> (
                  match Yojson.Safe.from_string l with
                  | j -> if f j then follow () else Ok ()
                  | exception Yojson.Json_error _ -> Ok ())
            in
            follow ()

let actor_of_env = function
  | None | Some "" | Some "user" -> Ok "user"
  | Some "agent" -> Ok "agent"
  | Some _ -> Error "MOTH_ACTOR must be user or agent"

type invocation = { cmd : string; args : string list; opts : (string * string) list; json : bool }

let is_option a = String.length a > 1 && a.[0] = '-' && not (match a.[1] with '0' .. '9' | '.' -> true | _ -> false)

let parse_args argv =
  let set k v opts = List.remove_assoc k opts @ [ (k, v) ] in
  let rec go words opts json = function
    | [] -> ( match List.rev words with [] -> Error "no command. run: moth help" | cmd :: args -> Ok { cmd; args; opts; json })
    (* the program's own arguments, whatever they look like, follow launch's path *)
    | (_ :: _) as game_args when (match words with [ _; "launch" ] -> true | _ -> false) ->
        Ok { cmd = "launch"; args = List.hd words :: game_args; opts; json }
    | "--json" :: rest -> go words opts true rest
    | "--all" :: rest -> go words (set "all" "true" opts) json rest
    | (("--bottle" | "-n" | "--from" | "--near" | "--after" | "--at" | "--kind") as o) :: rest -> (
        match rest with
        | v :: rest -> go words (set (if o = "-n" then "n" else String.sub o 2 (String.length o - 2)) v opts) json rest
        | [] -> Error (o ^ " needs a value"))
    | a :: _ when is_option a -> Error (sprintf "unknown option: %s. run: moth help" a)
    | a :: rest -> go (a :: words) opts json rest
  in
  go [] [] false argv
