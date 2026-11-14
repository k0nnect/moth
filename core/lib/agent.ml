let instructions =
  {|you are the agent built into moth, a memory scanner and editor for single-player games. the person is playing a game and hunting for values in it (health, money, a stat) so they can change them. you and they share one hunt: what they click in moth's window and what you run both act on it, and both show in the window.

how you work
- you drive moth with the moth command in the shell. moth help lists every command. moth status says what is attached and where the hunt stands. moth log lists what has been done, by them and by you.
- moth commands are approved in advance. run them without asking, including ones that write to the game (moth set). every write can be put back with moth undo. say what you did in a line.
- a note that starts with [moth] is what the person just did in the window. they did not type it. answer with the next move in one or two short lines: what to do in the game, and which button or command comes after. if the next step is yours, do it.
- the person never chooses a data type and you never ask for one. moth searches every way a number can be stored.
- to find a value: moth find <the number on screen>, have them change it in the game, moth next <the new number>, until few are left. with no number to read: moth snapshot, then moth next down, up, same or changed. moth results lists what is left, moth keep <#n> <name> keeps one, moth set <name> <number> changes it.
- when you write to a row by its number, add --at with the address moth results showed for it, so the write cannot land on another row.
- be brief. no headings. they are in the middle of a game.

limits
- single-player and offline play only. if they ask for help in an online or multiplayer game, or with getting around anti-cheat, say no and say why. moth refuses to attach when it sees a known anti-cheat. do not look for a way around that.
- use nothing but moth on the game: no other tool on its process, nothing loaded into it.
- when moth says something failed, tell them in moth's words. never assume a step worked.
|}

let first_prompt = "moth just opened. run moth status, then tell me in a line or two where things stand and what to do next."

(* in single quotes for sh, a single quote inside one written '\'' *)
let quote s =
  let b = Buffer.create (String.length s + 2) in
  Buffer.add_char b '\'';
  String.iter (fun c -> if c = '\'' then Buffer.add_string b "'\\''" else Buffer.add_char b c) s;
  Buffer.add_char b '\'';
  Buffer.contents b

let env ~home ~moth ~path = [ ("MOTH_ACTOR", "agent"); ("MOTH_HOME", home); ("PATH", Filename.dirname moth ^ ":" ^ path) ]

let settings ~home ~moth ~path ~dir : Yojson.Safe.t =
  `Assoc
    [
      ("env", `Assoc (List.map (fun (k, v) -> (k, `String v)) (env ~home ~moth ~path)));
      ("permissions", `Assoc [ ("allow", `List [ `String "Bash(moth *)" ]) ]);
      ( "hooks",
        `Assoc
          [
            ( "Stop",
              `List
                [
                  `Assoc
                    [
                      ( "hooks",
                        `List
                          [
                            `Assoc
                              [
                                ("type", `String "command");
                                ("command", `String (Printf.sprintf "%s wait --session %s" (quote moth) (quote dir)));
                                ("asyncRewake", `Bool true);
                                ("timeout", `Int 86400);
                              ];
                          ] );
                    ];
                ] );
          ] );
    ]

let command ~claude ~dir = [| claude; "--settings"; Filename.concat dir "settings.json"; "--append-system-prompt"; instructions; first_prompt |]

let mkdir d = try Unix.mkdir d 0o700 with Unix.Unix_error (Unix.EEXIST, _, _) -> ()

let write_private path text =
  let fd = Unix.openfile path [ Unix.O_WRONLY; Unix.O_CREAT; Unix.O_TRUNC ] 0o600 in
  Fun.protect
    ~finally:(fun () -> Unix.close fd)
    (fun () ->
      Unix.fchmod fd 0o600;
      let n = String.length text in
      if Unix.write_substring fd text 0 n <> n then failwith "short write")

let rec rm p =
  match Unix.lstat p with
  | exception Unix.Unix_error _ -> ()
  | { st_kind = S_DIR; _ } ->
      Array.iter (fun f -> rm (Filename.concat p f)) (try Sys.readdir p with Sys_error _ -> [||]);
      (try Unix.rmdir p with Unix.Unix_error _ -> ())
  | _ -> ( try Sys.remove p with Sys_error _ -> ())

let alive pid = match Unix.kill pid 0 with () -> true | exception Unix.Unix_error (Unix.EPERM, _, _) -> true | exception Unix.Unix_error _ -> false

(* a process id: decimal digits, above 0 *)
let session_pid name =
  match int_of_string_opt name with Some p when p > 0 && String.for_all (function '0' .. '9' -> true | _ -> false) name -> Some p | _ -> None

let prepare ~home ~moth ~path ~pid =
  try
    mkdir home;
    let agent = Filename.concat home "agent" in
    mkdir agent;
    mkdir (Filename.concat agent "work");
    Array.iter
      (fun name -> match session_pid name with Some p when p <> pid && not (alive p) -> rm (Filename.concat agent name) | _ -> ())
      (Sys.readdir agent);
    let dir = Filename.concat agent (string_of_int pid) in
    rm dir;
    mkdir dir;
    write_private (Filename.concat dir "settings.json") (Yojson.Safe.pretty_to_string (settings ~home ~moth ~path ~dir) ^ "\n");
    write_private (Filename.concat dir "instructions.md") instructions;
    Ok dir
  with Unix.Unix_error (e, _, p) -> Error (Printf.sprintf "cannot prepare the agent's folder: %s: %s" p (Unix.error_message e)) | Sys_error m | Failure m -> Error ("cannot prepare the agent's folder: " ^ m)
let remove_session = rm
