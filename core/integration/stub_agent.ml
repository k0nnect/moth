(* The stand-in for Claude Code in the end-to-end scenarios (MOTH_CLAUDE points at it). Started as
   moth agent starts claude: stub_agent --settings <file> --append-system-prompt <text> <prompt>.
   It writes its arguments to $MOTH_HOME/stub-args, one per line (a long argument as its first
   line only), runs the Stop hook's command from the settings file the way Claude Code does, and
   prints "stub agent up" once that has started. Each time the command exits 2 it appends what the
   command wrote to stderr to $MOTH_HOME/stub-notes and runs it again; on any other exit it stops
   running it. It ends when its stdin ends or it reads a line "bye", and kills the command it is
   running first (the command is in a group of its own, so moth wait goes with it). *)

let home () = match Sys.getenv_opt "MOTH_HOME" with Some h when h <> "" -> h | _ -> prerr_endline "stub_agent: MOTH_HOME is not set"; exit 2
let file name = Filename.concat (home ()) name

let write_file path text = Out_channel.with_open_bin path (fun oc -> Out_channel.output_string oc text)
let append_file path text = Out_channel.with_open_gen [ Open_wronly; Open_creat; Open_append; Open_binary ] 0o644 path (fun oc -> Out_channel.output_string oc text)
let first_line s = match String.index_opt s '\n' with Some i -> String.sub s 0 i | None -> s

let rec settings_path = function "--settings" :: p :: _ -> Some p | _ :: rest -> settings_path rest | [] -> None

(* hooks.Stop[0].hooks[0].command *)
let hook_command settings =
  let open Yojson.Safe.Util in
  let j = Yojson.Safe.from_file settings in
  j |> member "hooks" |> member "Stop" |> index 0 |> member "hooks" |> index 0 |> member "command" |> to_string

let err_file () = file "stub-hook-err"

(* runs the command in a group of its own, stdin from /dev/null, stdout dropped, stderr to a file *)
let start command =
  let err = Unix.openfile (err_file ()) [ Unix.O_WRONLY; Unix.O_CREAT; Unix.O_TRUNC ] 0o600 in
  match Unix.fork () with
  | 0 ->
      (try
         ignore (Unix.setsid ());
         let null_in = Unix.openfile "/dev/null" [ Unix.O_RDONLY ] 0 and null_out = Unix.openfile "/dev/null" [ Unix.O_WRONLY ] 0 in
         Unix.dup2 null_in Unix.stdin;
         Unix.dup2 null_out Unix.stdout;
         Unix.dup2 err Unix.stderr;
         Unix.execv "/bin/sh" [| "/bin/sh"; "-c"; command |]
       with _ -> ());
      exit 127
  | pid ->
      Unix.close err;
      pid

let kill_group pid = try Unix.kill (-pid) Sys.sigkill with Unix.Unix_error _ -> ( try Unix.kill pid Sys.sigkill with Unix.Unix_error _ -> ())

let () =
  let args = List.tl (Array.to_list Sys.argv) in
  write_file (file "stub-args") (String.concat "" (List.map (fun a -> first_line a ^ "\n") args));
  let command =
    match settings_path args with
    | None -> prerr_endline "stub_agent: no --settings"; exit 2
    | Some p -> ( try hook_command p with e -> Printf.eprintf "stub_agent: %s: %s\n" p (Printexc.to_string e); exit 2)
  in
  let running = ref (Some (start command)) in
  print_endline "stub agent up";
  let finish () =
    (match !running with Some pid -> kill_group pid; ignore (try Unix.waitpid [] pid with Unix.Unix_error _ -> (0, Unix.WEXITED 0)) | None -> ());
    exit 0
  in
  let reap () =
    match !running with
    | None -> ()
    | Some pid -> (
        match Unix.waitpid [ Unix.WNOHANG ] pid with
        | 0, _ -> ()
        | _, Unix.WEXITED 2 ->
            append_file (file "stub-notes") (In_channel.with_open_bin (err_file ()) In_channel.input_all ^ "\n");
            running := Some (start command)
        | _ -> running := None
        | exception Unix.Unix_error _ -> running := None)
  in
  Sys.set_signal Sys.sigterm (Sys.Signal_handle (fun _ -> finish ()));
  Sys.set_signal Sys.sighup (Sys.Signal_handle (fun _ -> finish ()));
  let buf = Buffer.create 64 and chunk = Bytes.create 256 in
  let rec loop () =
    reap ();
    (match Unix.select [ Unix.stdin ] [] [] 0.1 with
    | [], _, _ -> ()
    | _ -> (
        match Unix.read Unix.stdin chunk 0 256 with
        | 0 -> finish ()
        | n ->
            Buffer.add_subbytes buf chunk 0 n;
            if List.exists (fun l -> String.trim l = "bye") (String.split_on_char '\n' (Buffer.contents buf)) then finish ()
        | exception Unix.Unix_error (Unix.EINTR, _, _) -> ()
        | exception Unix.Unix_error _ -> finish ())
    | exception Unix.Unix_error (Unix.EINTR, _, _) -> ());
    loop ()
  in
  loop ()
