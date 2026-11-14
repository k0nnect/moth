(* The stand-in for Claude Code in the end-to-end scenarios (MOTH_CLAUDE points at it). Started as
   moth agent starts claude: stub_agent --settings <file> --append-system-prompt <text> <prompt>.
   It writes its arguments to $MOTH_HOME/stub-args, one per line (a long argument as its first
   line only), and reads the commands of the settings' UserPromptSubmit and Stop hooks: either
   missing, or the two different, and it says so on stderr and exits 1. It prints "stub agent up",
   then runs turns the way Claude Code does. A turn starts the UserPromptSubmit command; after the
   turn's length (STUB_TURN seconds, default 0.3) it starts the Stop command too. Neither waits for
   the other, and a command still running from an earlier turn is left running. Each command
   runs with /bin/sh -c in a process group of its own, stdin /dev/null, stdout dropped, stderr to
   a file of its own, and the settings file's "env" entries and HOME as its whole environment, as
   Claude Code applies the settings' env; the entries are written to $MOTH_HOME/stub-env
   (KEY=VALUE lines), and each start adds a line to $MOTH_HOME/stub-runs, the hook's event. When
   any command it started exits 2, what it wrote to stderr is one note, appended to
   $MOTH_HOME/stub-notes, and a new turn starts, once the turn under way (if any) has ended, as
   Claude Code delivers a note that comes during a turn when the turn ends. A command that exits
   any other way is only reaped. It ends when its stdin ends or it reads a line "bye", or on
   SIGTERM or SIGHUP, and first kills the process group of every command still running, as Claude
   Code hangs up its hooks when it ends.
   STUB_FIRST_DELAY=<seconds>: the first turn starts that long after "stub agent up" (Claude Code
   loading before its first prompt is submitted; default at once).
   STUB_INTERRUPT=1: every turn is interrupted, so the Stop command is never run (Claude Code runs
   no Stop hook after Escape): the only waiters are those armed as a turn starts.
   STUB_DEAF=1: it ignores SIGHUP and never reads its stdin, as an agent that does not notice its
   terminal going; only a kill ends it, and its commands are left to end by themselves. *)

let home () = match Sys.getenv_opt "MOTH_HOME" with Some h when h <> "" -> h | _ -> prerr_endline "stub_agent: MOTH_HOME is not set"; exit 2
let file name = Filename.concat (home ()) name

let write_file path text = Out_channel.with_open_bin path (fun oc -> Out_channel.output_string oc text)
let append_file path text = Out_channel.with_open_gen [ Open_wronly; Open_creat; Open_append; Open_binary ] 0o644 path (fun oc -> Out_channel.output_string oc text)
let first_line s = match String.index_opt s '\n' with Some i -> String.sub s 0 i | None -> s

let rec settings_path = function "--settings" :: p :: _ -> Some p | _ :: rest -> settings_path rest | [] -> None

(* hooks.<event>[0].hooks[0].command, or None when the settings give none *)
let hook_command settings event =
  let open Yojson.Safe.Util in
  try Some (settings |> member "hooks" |> member event |> index 0 |> member "hooks" |> index 0 |> member "command" |> to_string) with Type_error _ | Undefined _ -> None

(* the settings file's env entries as KEY=VALUE *)
let settings_env settings =
  let open Yojson.Safe.Util in
  settings |> member "env" |> to_assoc |> List.map (fun (k, v) -> k ^ "=" ^ to_string v)

let seconds name ~default = match Option.bind (Sys.getenv_opt name) float_of_string_opt with Some d when d >= 0. -> d | _ -> default

(* a hook's command running: its process, its stderr's file *)
type hook = { pid : int; err : string }

let started = ref 0

(* starts [command] for [event] in a group of its own, stdin from /dev/null, stdout dropped, stderr to a file *)
let start ~env ~event command =
  append_file (file "stub-runs") (event ^ "\n");
  incr started;
  let err = file (Printf.sprintf "stub-hook-%d.err" !started) in
  let fd = Unix.openfile err [ Unix.O_WRONLY; Unix.O_CREAT; Unix.O_TRUNC ] 0o600 in
  match Unix.fork () with
  | 0 ->
      (try
         Sys.set_signal Sys.sighup Sys.Signal_default;  (* a deaf stand-in's ignoring is its own *)
         ignore (Unix.setsid ());
         let null_in = Unix.openfile "/dev/null" [ Unix.O_RDONLY ] 0 and null_out = Unix.openfile "/dev/null" [ Unix.O_WRONLY ] 0 in
         Unix.dup2 null_in Unix.stdin;
         Unix.dup2 null_out Unix.stdout;
         Unix.dup2 fd Unix.stderr;
         Unix.execve "/bin/sh" [| "/bin/sh"; "-c"; command |] env
       with _ -> ());
      exit 127
  | pid ->
      Unix.close fd;
      { pid; err }

let kill_group pid = try Unix.kill (-pid) Sys.sigkill with Unix.Unix_error _ -> ( try Unix.kill pid Sys.sigkill with Unix.Unix_error _ -> ())
let forget h = try Sys.remove h.err with Sys_error _ -> ()

let () =
  let args = List.tl (Array.to_list Sys.argv) in
  write_file (file "stub-args") (String.concat "" (List.map (fun a -> first_line a ^ "\n") args));
  let path = match settings_path args with None -> prerr_endline "stub_agent: no --settings"; exit 2 | Some p -> p in
  let settings, vars =
    try
      let j = Yojson.Safe.from_file path in
      (j, settings_env j)
    with e -> Printf.eprintf "stub_agent: %s: %s\n" path (Printexc.to_string e); exit 2
  in
  let command event = match hook_command settings event with Some c -> c | None -> Printf.eprintf "stub_agent: %s has no %s hook\n" path event; exit 1 in
  let on_prompt = command "UserPromptSubmit" and on_stop = command "Stop" in
  if on_prompt <> on_stop then (
    Printf.eprintf "stub_agent: the UserPromptSubmit and Stop hooks in %s run different commands: %S and %S\n" path on_prompt on_stop;
    exit 1);
  write_file (file "stub-env") (String.concat "" (List.map (fun v -> v ^ "\n") vars));
  let env = Array.of_list (("HOME=" ^ Option.value (Sys.getenv_opt "HOME") ~default:"/") :: vars) in
  let turn_length = seconds "STUB_TURN" ~default:0.3 in
  let interrupt = Sys.getenv_opt "STUB_INTERRUPT" = Some "1" in
  let deaf = Sys.getenv_opt "STUB_DEAF" = Some "1" in
  print_endline "stub agent up";
  let first_at = Unix.gettimeofday () +. seconds "STUB_FIRST_DELAY" ~default:0. in
  let hooks = ref [] in
  (* when the turn under way ends; a turn is due (the first, or one a note started) *)
  let turn_ends = ref None and turn_due = ref false and first_due = ref true in
  let finish () =
    List.iter
      (fun h ->
        kill_group h.pid;
        ignore (try Unix.waitpid [] h.pid with Unix.Unix_error _ -> (0, Unix.WEXITED 0));
        forget h)
      !hooks;
    hooks := [];
    exit 0
  in
  let reap () =
    hooks :=
      List.filter
        (fun h ->
          match Unix.waitpid [ Unix.WNOHANG ] h.pid with
          | 0, _ -> true
          | _, Unix.WEXITED 2 ->
              append_file (file "stub-notes") (In_channel.with_open_bin h.err In_channel.input_all ^ "\n");
              turn_due := true;
              forget h;
              false
          | _ ->
              forget h;
              false
          | exception Unix.Unix_error _ ->
              forget h;
              false)
        !hooks
  in
  let turns () =
    let now = Unix.gettimeofday () in
    if !first_due && now >= first_at then (
      first_due := false;
      turn_due := true);
    (match !turn_ends with
    | Some t when now >= t ->
        turn_ends := None;
        if not interrupt then hooks := start ~env ~event:"Stop" on_stop :: !hooks
    | _ -> ());
    if !turn_ends = None && !turn_due then (
      turn_due := false;
      hooks := start ~env ~event:"UserPromptSubmit" on_prompt :: !hooks;
      turn_ends := Some (now +. turn_length))
  in
  Sys.set_signal Sys.sigterm (Sys.Signal_handle (fun _ -> finish ()));
  Sys.set_signal Sys.sighup (if deaf then Sys.Signal_ignore else Sys.Signal_handle (fun _ -> finish ()));
  let buf = Buffer.create 64 and chunk = Bytes.create 256 in
  let rec loop () =
    reap ();
    turns ();
    (match Unix.select (if deaf then [] else [ Unix.stdin ]) [] [] 0.05 with
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
