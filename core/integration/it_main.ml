(* End to end in a bottle: the moth command, its daemon and the helper against the stand-in game
   (tests/target/target.cpp), all running in the test bottle MOTH_TEST_BOTTLE through WINE.
   Scenarios drive [bin/moth ... --json] as a subprocess and check its replies against what the
   target reports. One line per scenario; the exit code is 1 when any scenario fails.

     it_main.exe [--bin <dir>] [-v] [scenario numbers]   (every scenario when none are given;
                                                     -v shows each moth command and its output)

   Runs under a fresh MOTH_HOME in /tmp, and stops the daemon and the targets afterwards, whatever
   happened. The home is removed when every scenario passed, and kept (for its logs) otherwise. *)

open Yojson.Safe.Util

let sprintf = Printf.sprintf
let failf fmt = Printf.ksprintf failwith fmt
let now = Unix.gettimeofday
let rec restart f = try f () with Unix.Unix_error (Unix.EINTR, _, _) -> restart f

(* the index just past the first [sub] in [s] *)
let after sub s =
  let n = String.length s and m = String.length sub in
  let rec from i = if i + m > n then None else if String.sub s i m = sub then Some (i + m) else from (i + 1) in
  from 0

let contains s sub = after sub s <> None

(* ---- where things are ---- *)

let bin = ref (Filename.concat (Sys.getcwd ()) "bin")
let home = ref ""
let exe name = Filename.concat !bin name
let bottle () = match Sys.getenv_opt "MOTH_TEST_BOTTLE" with Some b when b <> "" -> b | _ -> failwith "MOTH_TEST_BOTTLE is not set. run this through make."

(* ---- running a program with a deadline ---- *)

type ran = { code : int; out : string; err : string }

let run ~timeout argv =
  let out_r, out_w = Unix.pipe ~cloexec:true () in
  let err_r, err_w = Unix.pipe ~cloexec:true () in
  let null = Unix.openfile "/dev/null" [ Unix.O_RDONLY; Unix.O_CLOEXEC ] 0 in
  let pid = Fun.protect ~finally:(fun () -> List.iter Unix.close [ null; out_w; err_w ]) (fun () -> Unix.create_process argv.(0) argv null out_w err_w) in
  let out = Buffer.create 4096 and err = Buffer.create 256 and chunk = Bytes.create 65536 in
  let deadline = now () +. timeout in
  let opened = ref [ out_r; err_r ] in
  let close_all () = List.iter (fun fd -> try Unix.close fd with Unix.Unix_error _ -> ()) !opened in
  let drained fd =
    match restart (fun () -> Unix.read fd chunk 0 (Bytes.length chunk)) with
    | 0 -> true
    | n ->
        Buffer.add_subbytes (if fd = out_r then out else err) chunk 0 n;
        false
  in
  let rec pump () =
    if !opened = [] then true
    else
      let left = deadline -. now () in
      if left <= 0. then false
      else
        let ready, _, _ = restart (fun () -> Unix.select !opened [] [] left) in
        let done_ = List.filter drained ready in
        List.iter Unix.close done_;
        opened := List.filter (fun fd -> not (List.mem fd done_)) !opened;
        pump ()
  in
  let finished = Fun.protect ~finally:close_all (fun () -> try pump () with e -> (try Unix.kill pid Sys.sigkill with Unix.Unix_error _ -> ()); raise e) in
  if not finished then (try Unix.kill pid Sys.sigkill with Unix.Unix_error _ -> ());
  let code = match snd (restart (fun () -> Unix.waitpid [] pid)) with Unix.WEXITED c -> c | WSIGNALED s | WSTOPPED s -> 128 + abs s in
  if not finished then failf "%s did not finish within %.0fs" (String.concat " " (Array.to_list argv)) timeout;
  { code; out = Buffer.contents out; err = Buffer.contents err }

(* ---- the moth command ---- *)

(* -v: every moth command and what it printed, on stderr *)
let verbose = ref false

let moth_raw ?(timeout = 900.) args =
  let r = run ~timeout (Array.of_list ((exe "moth" :: args) @ [ "--json" ])) in
  if !verbose then Printf.eprintf "moth %s -> exit %d\n%s%s%!" (String.concat " " args) r.code r.out r.err;
  r

let moth ?timeout args =
  let r = moth_raw ?timeout args in
  if r.code <> 0 then failf "moth %s failed (exit %d): %s" (String.concat " " args) r.code (String.trim r.err)
  else try Yojson.Safe.from_string r.out with Yojson.Json_error m -> failf "moth %s printed something that isn't json (%s): %s" (String.concat " " args) m r.out

let find n = moth [ "find"; n ]
let next n = moth [ "next"; n ]
let notes o = List.map to_string (to_list (member "notes" o))
let show o = Yojson.Safe.to_string o

type row = { n : int; addr : int64; tag : string; shown : string }

let results ?(all = false) () =
  List.map
    (fun r -> { n = to_int (member "n" r); addr = Int64.of_string (to_string (member "addr" r)); tag = to_string (member "tag" r); shown = to_string (member "shown" r) })
    (to_list (moth ("results" :: (if all then [ "--all" ] else []))))

let show_rows rows = if rows = [] then "no results" else String.concat "; " (List.map (fun r -> sprintf "#%d %s 0x%Lx %s" r.n r.shown r.addr r.tag) rows)
let row_at rows (v : Target_ctl.var) tag = List.find_opt (fun r -> r.addr = v.addr && r.tag = tag) rows

let need_row rows (v : Target_ctl.var) tag =
  match row_at rows v tag with Some r -> r | None -> failf "no \"%s\" result at %s's address 0x%Lx. results: %s" tag v.name v.addr (show_rows rows)

let check ok fmt = Printf.ksprintf (fun m -> if not ok then failwith m) fmt

let expect t name want =
  let got = Target_ctl.get t name in
  check (got = want) "the target has %s = %s, expected %s" name got want

let expect_near t name want =
  let got = Target_ctl.get t name in
  check (match float_of_string_opt got with Some f -> Float.abs (f -. want) < 1e-4 | None -> false) "the target has %s = %s, expected %g" name got want

(* ---- one target per scenario ---- *)

let target_log () = Filename.concat !home "target.log"

(* what the latest attach said *)
let attach_said = ref ""

let with_target ?(name = "moth-target.exe") ?(filler_mb = 0) ?(filler_small_mb = 0) f =
  let t = Target_ctl.start ~filler_small_mb ~log:(target_log ()) ~exe:(exe name) ~filler_mb () in
  Fun.protect
    ~finally:(fun () -> Target_ctl.stop t)
    (fun () ->
      attach_said := to_string (member "message" (moth [ "attach"; "--bottle"; bottle (); name ]));
      f t)

let set_row (r : row) v = ignore (moth [ "set"; string_of_int r.n; v ])

(* ---- the scenarios ---- *)

(* 1: health is a float holding 100.0; the user only ever types whole numbers *)
let decimal_steps t =
  ignore (find "100");
  Target_ctl.set t "health" "85.3";
  ignore (next "85");
  Target_ctl.set t "health" "61.9";
  ignore (next "61");
  let r = need_row (results ()) (Target_ctl.var t "health") "decimal" in
  set_row r "999";
  expect t "health" "999"

let decimal_from_whole () = with_target decimal_steps

let money () =
  with_target (fun t ->
      ignore (find "132.50");
      Target_ctl.set t "money_cents" "9925";
      ignore (next "99.25");
      let r = need_row (results ()) (Target_ctl.var t "money_cents") "whole number ×100" in
      set_row r "999.99";
      expect t "money_cents" "99999")

let one_byte () =
  with_target (fun t ->
      ignore (find "30");
      Target_ctl.set t "ammo" "29";
      ignore (next "29");
      Target_ctl.set t "ammo" "12";
      ignore (next "12");
      let ammo = Target_ctl.var t "ammo" and tag = "whole number (1-byte)" in
      let rows = results () in
      ignore (need_row (if row_at rows ammo tag <> None then rows else results ~all:true ()) ammo tag))

let unaligned () =
  with_target (fun t ->
      ignore (find "4242");
      Target_ctl.set t "gems" "4300";
      let o = next "4300" in
      check (List.mem "showing_wide" (notes o)) "next 4300 is not showing wide matches: %s" (show o);
      ignore (need_row (results ()) (Target_ctl.var t "gems") "whole number (unaligned)"))

let fallback () =
  with_target (fun t ->
      ignore (find "100");
      Target_ctl.set t "health" "80";
      Target_ctl.set t "health_copy" "80";
      let o = next "77" in
      check (List.mem "fell_back_down" (notes o)) "next 77 did not fall back to went down: %s" (show o);
      let v = Target_ctl.var t "health_copy" and rows = results () in
      check (List.exists (fun r -> r.addr = v.addr) rows) "no result at health_copy's address 0x%Lx. results: %s" v.addr (show_rows rows))

(* the counts and groups a find, a next or status reports, to compare two of them *)
let counts o = (to_int (member "main" o), to_int (member "wide" o), to_list (member "groups" o))

let dead_end () =
  with_target (fun _ ->
      let f = find "100" in
      let o = next "12345" in
      check (to_bool (member "dead_end" o)) "next 12345 was not a dead end: %s" (show o);
      let hunt = member "hunt" (moth [ "status" ]) in
      check (counts hunt = counts f) "status after the dead end differs from the find. find: %s. status: %s" (show f) (show hunt))

let undo () =
  with_target (fun t ->
      decimal_steps t;
      ignore (moth [ "undo" ]);
      expect_near t "health" 61.9)

let bits32 () =
  with_target ~name:"moth-target32.exe" (fun t ->
      check (contains !attach_said "32-bit") "attach said \"%s\", not 32-bit" !attach_said;
      let attached = member "attached" (moth [ "status" ]) in
      check (attached <> `Null && to_int (member "bits" attached) = 32) "status does not show a 32-bit target: %s" (show attached);
      ignore (find "100");
      Target_ctl.set t "health_copy" "64";
      ignore (next "64");
      ignore (need_row (results ()) (Target_ctl.var t "health_copy") "whole number"))

let tiny_common () =
  with_target ~filler_small_mb:256 (fun t ->
      let o = find "3" in
      check (to_int (member "main" o) >= 8_000_000) "find 3 found %d main matches, expected at least 8,000,000: %s" (to_int (member "main" o)) (show o);
      let hunt = member "hunt" (moth [ "status" ]) in
      let untracked = List.filter (fun g -> to_string (member "tier" g) = "main" && not (to_bool (member "tracked" g))) (to_list (member "groups" hunt)) in
      check (untracked = []) "status shows main groups untracked: %s" (show (`List untracked));
      Target_ctl.set t "lives" "2";
      ignore (next "2");
      Target_ctl.set t "lives" "1";
      ignore (next "1");
      ignore (need_row (results ()) (Target_ctl.var t "lives") "whole number"))

let gib = 1073741824.
let max_s_per_gb = 3.0

let speed () =
  with_target ~filler_mb:1024 (fun _ ->
      let o = find "100" in
      let ms = to_number (member "ms" o) and gb = to_number (member "bytes" o) /. gib in
      check (gb >= 1.) "find 100 read only %.3f GB, less than the 1 GB filler" gb;
      let s_per_gb = ms /. 1000. /. gb in
      Printf.printf "scan: %.2f s/GB (%.2f GB in %.0f ms)\n%!" s_per_gb gb ms;
      let t0 = now () in
      let n = next "100" in
      Printf.printf "next 100: %.0f ms in the helper, %.0f ms end to end\n%!" (to_number (member "ms" n)) ((now () -. t0) *. 1000.);
      check (s_per_gb <= max_s_per_gb) "the scan took %.2f s/GB, over the %.1f s/GB bar" s_per_gb max_s_per_gb)

let game_closing () =
  with_target (fun t ->
      ignore (find "100");
      Target_ctl.quit t;
      let r = moth_raw [ "next"; "85" ] in
      let said = String.trim r.err and want = "the game closed. attach again once it's running." in
      check (r.code = 1 && said = want) "next 85 after the game closed gave exit %d and \"%s\", expected exit 1 and \"%s\"" r.code said want)

exception Skipped of string

let anticheat () = raise (Skipped "not run in a bottle; the daemon's unit tests cover it with a fake helper")

let scenarios =
  [
    (1, "decimal found from a whole number", decimal_from_whole);
    (2, "money typed with decimals", money);
    (3, "one-byte value", one_byte);
    (4, "unaligned value surfaces on its own", unaligned);
    (5, "fallback to a relation", fallback);
    (6, "dead end changes nothing", dead_end);
    (7, "undo", undo);
    (8, "32-bit target", bits32);
    (9, "a tiny, common number", tiny_common);
    (10, "speed", speed);
    (11, "the game closing", game_closing);
    (12, "anti-cheat names", anticheat);
  ]

(* ---- the daemon's lifetime ---- *)

(* the n in a core.log line "... serving <socket> (pid n)" *)
let logged_pid line =
  match after "(pid " line with
  | None -> None
  | Some i -> ( match String.index_from_opt line i ')' with Some j -> int_of_string_opt (String.sub line i (j - i)) | None -> None)

(* every daemon that served this home *)
let daemon_pids () =
  match In_channel.with_open_bin (Filename.concat !home "core.log") In_channel.input_all with
  | exception Sys_error _ -> []
  | log -> List.filter_map logged_pid (String.split_on_char '\n' log)

let alive pid = match Unix.kill pid 0 with () -> true | exception Unix.Unix_error _ -> false

let wait_gone pid ~timeout =
  let deadline = now () +. timeout in
  let rec go () = if not (alive pid) then true else if now () > deadline then false else (Unix.sleepf 0.1; go ()) in
  go ()

(* stops the daemon (which closes the helper) and makes sure it has exited; false when it had
   to be killed *)
let stop_daemon () =
  let stopped = match moth_raw ~timeout:60. [ "stop" ] with r -> r.code = 0 | exception Failure _ -> false in
  List.fold_left
    (fun clean pid ->
      if wait_gone pid ~timeout:15. then clean
      else (
        (try Unix.kill pid Sys.sigkill with Unix.Unix_error _ -> ());
        Printf.printf "moth daemon (pid %d) did not stop, so it was killed\n%!" pid;
        false))
    stopped (daemon_pids ())

let rec rm p =
  match Unix.lstat p with
  | exception Unix.Unix_error _ -> ()
  | { st_kind = S_DIR; _ } ->
      Array.iter (fun f -> rm (Filename.concat p f)) (Sys.readdir p);
      Unix.rmdir p
  | _ -> Sys.remove p

let () =
  let rec opts picked = function
    | "--bin" :: dir :: rest ->
        bin := (if Filename.is_relative dir then Filename.concat (Sys.getcwd ()) dir else dir);
        opts picked rest
    | "-v" :: rest ->
        verbose := true;
        opts picked rest
    | n :: rest -> (
        match int_of_string_opt n with
        | Some k when List.exists (fun (j, _, _) -> j = k) scenarios -> opts (k :: picked) rest
        | _ ->
            prerr_endline ("usage: it_main [--bin <dir>] [-v] [scenario numbers 1 to " ^ string_of_int (List.length scenarios) ^ "]");
            exit 2)
    | [] -> List.rev picked
  in
  let picked = opts [] (List.tl (Array.to_list Sys.argv)) in
  let picked = if picked = [] then List.map (fun (k, _, _) -> k) scenarios else picked in
  Sys.catch_break true;
  Sys.set_signal Sys.sigterm (Sys.Signal_handle (fun _ -> raise Sys.Break));
  (* under /tmp rather than $TMPDIR: macOS caps unix socket paths at 104 bytes *)
  home := sprintf "/tmp/moth-it-%d" (Unix.getpid ());
  rm !home;
  Unix.mkdir !home 0o700;
  Unix.putenv "MOTH_HOME" !home;
  Unix.putenv "MOTH_HELPER" (exe "moth-helper.exe");
  let failed = ref 0 in
  let result k title = function
    | Ok () -> Printf.printf "ok    %2d %s\n%!" k title
    | Error (`Skipped why) -> Printf.printf "skip  %2d %s: %s\n%!" k title why
    | Error (`Failed why) ->
        incr failed;
        Printf.printf "fail  %2d %s: %s\n%!" k title why
  in
  let clean =
    match
      List.iter
        (fun (k, title, f) ->
          if List.mem k picked then
            result k title
              (match f () with
              | () -> Ok ()
              | exception Skipped why -> Error (`Skipped why)
              | exception Failure m -> Error (`Failed m)
              | exception Sys.Break -> raise Sys.Break
              | exception e -> Error (`Failed (Printexc.to_string e))))
        scenarios
    with
    | () -> stop_daemon ()
    | exception Sys.Break ->
        ignore (stop_daemon ());
        Printf.printf "interrupted. logs kept in %s\n%!" !home;
        exit 130
  in
  if not clean then incr failed;
  if !failed = 0 then rm !home else Printf.printf "logs kept in %s\n%!" !home;
  exit (if !failed = 0 then 0 else 1)
