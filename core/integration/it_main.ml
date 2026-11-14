(* End to end in a bottle: the moth command, its daemon and the helper against the stand-in game
   (tests/target/target.cpp), all running in the test bottle MOTH_TEST_BOTTLE through WINE.
   Scenarios drive [bin/moth --json ...] as a subprocess and check its replies against what the
   target reports; a few speak to the daemon's socket themselves, and eight drive the window
   through <dir>/../build/app/it_app. One line per scenario; the exit code is 1 when any
   scenario fails.

     it_main.exe [--bin <dir>] [-v] [scenario numbers]   (every scenario when none are given;
                                         -v shows each moth command and its output, and it_app's)

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

(* the environment with [vars] set, each replacing the value it had *)
let env_with vars =
  let unset e = not (List.exists (fun (k, _) -> String.starts_with ~prefix:(k ^ "=") e) vars) in
  Array.of_list (List.filter unset (Array.to_list (Unix.environment ())) @ List.map (fun (k, v) -> k ^ "=" ^ v) vars)

(* ---- running a program with a deadline ---- *)

type ran = { code : int; out : string; err : string }

let run ?env ~timeout argv =
  let out_r, out_w = Unix.pipe ~cloexec:true () in
  let err_r, err_w = Unix.pipe ~cloexec:true () in
  let null = Unix.openfile "/dev/null" [ Unix.O_RDONLY; Unix.O_CLOEXEC ] 0 in
  let spawn () = match env with None -> Unix.create_process argv.(0) argv null out_w err_w | Some e -> Unix.create_process_env argv.(0) argv e null out_w err_w in
  let pid = Fun.protect ~finally:(fun () -> List.iter Unix.close [ null; out_w; err_w ]) spawn in
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

(* --json first: after launch's path everything is the game's own *)
let moth_raw ?env ?(timeout = 900.) args =
  let r = run ?env ~timeout (Array.of_list (exe "moth" :: "--json" :: args)) in
  if !verbose then Printf.eprintf "moth %s -> exit %d\n%s%s%!" (String.concat " " args) r.code r.out r.err;
  r

let moth ?env ?timeout args =
  let r = moth_raw ?env ?timeout args in
  if r.code <> 0 then failf "moth %s failed (exit %d): %s" (String.concat " " args) r.code (String.trim r.err)
  else try Yojson.Safe.from_string r.out with Yojson.Json_error m -> failf "moth %s printed something that isn't json (%s): %s" (String.concat " " args) m r.out

let find n = moth [ "find"; n ]
let next n = moth [ "next"; n ]
let notes o = List.map to_string (to_list (member "notes" o))
let show o = Yojson.Safe.to_string o

type row = { n : int; addr : int64; tag : string; shown : string; away : int option }
type page = { rows : row list; total : int }

let hex a = sprintf "0x%Lx" a
let addr_of o = Int64.of_string (to_string (member "addr" o))

(* one page of results: [n] rows from row [from] (moth's defaults when not given), the wide
   tier too under [all], or the rows near [near] *)
let page ?(all = false) ?n ?from ?near () =
  let given flag = function Some v -> [ flag; v ] | None -> [] in
  let o =
    moth
      (("results" :: (if all then [ "--all" ] else []))
      @ given "-n" (Option.map string_of_int n)
      @ given "--from" (Option.map string_of_int from)
      @ given "--near" near)
  in
  let row r =
    { n = to_int (member "n" r); addr = addr_of r; tag = to_string (member "tag" r); shown = to_string (member "shown" r); away = to_int_option (member "away" r) }
  in
  { rows = List.map row (to_list (member "rows" o)); total = to_int (member "total" o) }

let results ?all () = (page ?all ()).rows

let show_rows rows =
  let away r = match r.away with Some d -> sprintf " away %d" d | None -> "" in
  if rows = [] then "no results" else String.concat "; " (List.map (fun r -> sprintf "#%d %s 0x%Lx %s%s" r.n r.shown r.addr r.tag (away r)) rows)
let row_at rows (v : Target_ctl.var) tag = List.find_opt (fun r -> r.addr = v.addr && r.tag = tag) rows

let need_row rows (v : Target_ctl.var) tag =
  match row_at rows v tag with Some r -> r | None -> failf "no \"%s\" result at %s's address 0x%Lx. results: %s" tag v.name v.addr (show_rows rows)

let check ok fmt = Printf.ksprintf (fun m -> if not ok then failwith m) fmt

let expect t name want =
  let got = Target_ctl.get t name in
  check (got = want) "the target has %s = %s, expected %s" name got want

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

(* Runs [f] against a daemon of its own, started with [env] in its environment. The daemon reads
   its environment once, when the moth command starts it, so the running one is stopped first;
   the next moth command starts the new one. Afterwards [env] is put back as it was and that
   daemon is stopped too, so the next scenario starts one without [env]. *)
let with_daemon_env env f =
  let saved = List.map (fun (k, _) -> (k, Option.value (Sys.getenv_opt k) ~default:"")) env in
  let restore () = List.iter (fun (k, v) -> Unix.putenv k v) saved in
  check (stop_daemon ()) "moth's daemon did not stop when asked";
  List.iter (fun (k, v) -> Unix.putenv k v) env;
  match f () with
  | () ->
      restore ();
      check (stop_daemon ()) "moth's daemon did not stop when asked"
  | exception e ->
      restore ();
      ignore (stop_daemon ());
      raise e

(* ---- one target per scenario ---- *)

let target_log () = Filename.concat !home "target.log"

(* what the latest attach said *)
let attach_said = ref ""

(* [attach] false: the scenario attaches itself *)
let with_target ?(name = "moth-target.exe") ?(attach = true) ?(filler_mb = 0) ?(filler_small_mb = 0) ?(filler_tiny_mb = 0) ?(filler_half_mb = 0) f =
  let t = Target_ctl.start ~filler_small_mb ~filler_tiny_mb ~filler_half_mb ~log:(target_log ()) ~exe:(exe name) ~filler_mb () in
  Fun.protect
    ~finally:(fun () -> Target_ctl.stop t)
    (fun () ->
      if attach then attach_said := to_string (member "message" (moth [ "attach"; "--bottle"; bottle (); name ]));
      f t)

let set_row (r : row) v = ignore (moth [ "set"; string_of_int r.n; v ])

(* The modules of the running target [exe_name], asked of a helper of our own in the bottle (the
   daemon's helper is its own). attach checks them for anti-cheat and refuses when they can't be
   read; this shows the list is really there under CrossOver: the program itself and [also].
   (For a 32-bit program CrossOver lists only the 64-bit WoW64 modules, not kernel32.dll.) *)
let check_modules ?(also = []) exe_name =
  let open Moth_core in
  let log = Filename.concat !home "helper.log" in
  let launch = Helper_conn.wine_launcher ~bottle:(bottle ()) ~helper_exe:(exe "moth-helper.exe") in
  match Helper_conn.start ~launch ~label:"the test bottle" ~log () with
  | Error m -> failf "couldn't start a helper to list modules: %s" m
  | Ok h ->
      Fun.protect ~finally:h.close (fun () ->
          let fail_with what (e : Helper_api.err) = failf "%s: %s %s" what e.code e.message in
          let procs = match h.ps () with Ok p -> p | Error e -> fail_with "ps" e in
          let pid =
            match List.find_opt (fun (_, n) -> String.lowercase_ascii n = exe_name) procs with
            | Some (p, _) -> p
            | None -> failf "%s isn't running in the bottle" exe_name
          in
          match h.modules pid with
          | Error e -> fail_with ("the modules of " ^ exe_name) e
          | Ok mods ->
              let lower = List.map String.lowercase_ascii mods in
              check (List.for_all (fun m -> List.mem m lower) (exe_name :: also)) "the modules of %s are: %s" exe_name (String.concat ", " mods))

(* ---- the scenarios ---- *)

(* 1: health is a float holding 100.0; the user only ever types whole numbers. Gives what the
   game showed for health just before moth wrote it. *)
let decimal_steps t =
  ignore (find "100");
  Target_ctl.set t "health" "85.3";
  ignore (next "85");
  Target_ctl.set t "health" "61.9";
  ignore (next "61");
  let before = Target_ctl.get t "health" in
  let r = need_row (results ()) (Target_ctl.var t "health") "decimal" in
  set_row r "999";
  expect t "health" "999";
  before

let decimal_from_whole () =
  with_target (fun t ->
      check_modules ~also:[ "kernel32.dll" ] "moth-target.exe";
      ignore (decimal_steps t))

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
      check (List.exists (fun r -> r.addr = v.addr) rows) "no result at health_copy's address 0x%Lx. results: %s" v.addr (show_rows rows);
      (* 77 was a slip for the 80 the game shows. Set again and typed, 80 still finds health, a
         decimal whose stored value is already 80: after a fallback it need not have moved *)
      Target_ctl.set t "health" "80";
      ignore (next "80");
      let rows = results () in
      ignore (need_row rows (Target_ctl.var t "health") "decimal");
      ignore (need_row rows v "whole number"))

(* the counts and groups a find, a next or status reports, to compare two of them *)
let counts o = (to_int (member "main" o), to_int (member "wide" o), to_list (member "groups" o))

let dead_end () =
  with_target (fun _ ->
      let f = find "100" in
      let o = next "12345" in
      check (to_bool (member "dead_end" o)) "next 12345 was not a dead end: %s" (show o);
      let hunt = member "hunt" (moth [ "status" ]) in
      check (counts hunt = counts f) "status after the dead end differs from the find. find: %s. status: %s" (show f) (show hunt);
      (* the helper did not narrow either: the number that is still true keeps every match *)
      let again = next "100" in
      check (counts again = counts f) "next 100 after the dead end differs from the find. find: %s. next: %s" (show f) (show again))

(* the bytes undo puts back are exactly the ones the game had *)
let undo () =
  with_target (fun t ->
      let before = decimal_steps t in
      ignore (moth [ "undo" ]);
      expect t "health" before)

let bits32 () =
  with_target ~name:"moth-target32.exe" (fun t ->
      check (contains !attach_said "32-bit") "attach said \"%s\", not 32-bit" !attach_said;
      check_modules "moth-target32.exe";
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

(* 13 to 15: the other widths, found by the number the game shows, then written through moth *)
let two_byte () =
  with_target (fun t ->
      ignore (find "7");
      Target_ctl.set t "level16" "9";
      ignore (next "9");
      let r = need_row (results ()) (Target_ctl.var t "level16") "whole number (2-byte)" in
      set_row r "12";
      expect t "level16" "12")

let eight_byte () =
  with_target (fun t ->
      ignore (find "100000");
      Target_ctl.set t "xp64" "100250";
      ignore (next "100250");
      let r = need_row (results ()) (Target_ctl.var t "xp64") "whole number (8-byte)" in
      set_row r "5000000000";
      expect t "xp64" "5000000000")

let eight_byte_decimal () =
  with_target (fun t ->
      ignore (find "85");
      Target_ctl.set t "morale" "70.6";
      ignore (next "70");
      let r = need_row (results ()) (Target_ctl.var t "morale") "decimal (8-byte)" in
      set_row r "99.5";
      expect t "morale" "99.5")

(* 16: the game frees one page in the middle of a dense hunt. Only the matches in that page are
   lost. A new search no longer walks that page at all (it is not committed memory any more, so
   the region list leaves it out), so it reads everything else and counts nothing unreadable. *)
let freed_page () =
  with_target ~filler_small_mb:64 (fun t ->
      let f = find "3" in
      let groups = List.length (to_list (member "groups" f)) in
      Target_ctl.free t (64 * 256 / 2);
      Target_ctl.set t "lives" "2";
      let n = next "2" in
      let lost = to_int (member "lost" n) in
      ignore (need_row (results ()) (Target_ctl.var t "lives") "whole number");
      check (lost > 0) "next 2 lost no matches, so it never met the freed page: %s" (show n);
      check (lost <= 1024 * groups) "next 2 lost %d matches, more than one page's worth (1024) for each of %d groups" lost groups;
      let again = find "3" in
      let unreadable = to_int (member "unreadable" again) and bytes = to_int (member "bytes" again) in
      check (unreadable = 0) "find 3 after the free couldn't read %d bytes, expected none: %s" unreadable (show again);
      check (bytes >= (64 lsl 20) - 4096) "find 3 after the free read only %d bytes, less than the filler around the page" bytes)

(* the count of the group [id] in a find's or a next's reply *)
let group_count o id =
  match List.find_opt (fun g -> to_string (member "id" g) = id) (to_list (member "groups" o)) with
  | Some g -> to_int (member "count" g)
  | None -> failf "no %s group in %s" id (show o)

(* the kept value called [name], from kept --json *)
let kept_one name =
  let ks = to_list (moth [ "kept" ]) in
  match List.find_opt (fun k -> to_string (member "name" k) = name) ks with Some k -> k | None -> failf "nothing kept as %s: %s" name (show (`List ks))

(* waits until the target reports [name] = [want], for a value the target works out by itself *)
let await t name want =
  let deadline = now () +. 5. in
  let rec go () =
    let got = Target_ctl.get t name in
    if got = want then ()
    else if now () > deadline then failf "the target still has %s = %s, expected it to become %s" name got want
    else (Unix.sleepf 0.005; go ())
  in
  go ()

(* 17: 1 among 256 MiB of small whole numbers, which read as floats are tiny (denormal) and are
   left out, so the float groups stay small and the next number is quick *)
let tiny_values () =
  with_target ~filler_tiny_mb:256 (fun t ->
      let points = Target_ctl.var t "points" in
      let f = find "1" in
      List.iter
        (fun id ->
          let c = group_count f id in
          check (c < 1000) "find 1 found %d %s matches, expected under 1,000" c id)
        [ "f32"; "f64" ];
      Target_ctl.set t "points" "0";
      let t0 = now () in
      let o = next "0" in
      let took = now () -. t0 in
      if !verbose then Printf.eprintf "find 1: %.0f ms in the helper. next 0: %.0f ms in the helper, %.0f ms end to end\n%!" (to_number (member "ms" f)) (to_number (member "ms" o)) (took *. 1000.);
      check (took < 3.) "next 0 took %.1f s end to end, expected under 3" took;
      match (page ~near:(hex points.addr) ()).rows with
      | first :: _ as rows -> check (first.addr = points.addr && first.away = Some 0) "the first row near points (0x%Lx) is not points' own, away 0. results: %s" points.addr (show_rows rows)
      | [] -> failf "no rows near points")

(* 18: two floats in range of both numbers; only the one the game changed stays *)
let still_float () =
  with_target (fun t ->
      let charge = Target_ctl.var t "charge" and drift = Target_ctl.var t "drift" in
      ignore (find "5");
      Target_ctl.set t "charge" "6";
      ignore (next "6");
      let rows = (page ~all:true ~n:200 ()).rows in
      let listed (v : Target_ctl.var) = List.exists (fun r -> r.addr = v.addr) rows in
      check (listed charge) "charge (0x%Lx) is not listed. results: %s" charge.addr (show_rows rows);
      check (not (listed drift)) "drift (0x%Lx), which did not move, is listed. results: %s" drift.addr (show_rows rows))

(* 19: the game works total out from base + bonus every 20 ms, so a write to it does not stick,
   and moth says so *)
let recalculated () =
  with_target (fun t ->
      ignore (find "6");
      Target_ctl.set t "bonus" "0";
      await t "total" "5";
      ignore (next "5");
      Target_ctl.set t "bonus" "1";
      await t "total" "6";
      ignore (next "6");
      let r = need_row (results ()) (Target_ctl.var t "total") "whole number" in
      let o = moth [ "set"; string_of_int r.n; "9" ] in
      let said = to_string (member "message" o) in
      check (not (to_bool (member "held" o))) "set #%d 9 says it held: %s" r.n (show o);
      let second = match String.split_on_char '\n' said with _ :: l :: _ -> l | _ -> "" in
      check (String.starts_with ~prefix:"the game put it straight back to 6" second) "set #%d 9 said %S" r.n said;
      expect t "total" "6")

(* 20: a value kept by name, the rows near it in a later hunt, and a set by name, undone *)
let keep_near () =
  with_target (fun t ->
      let xp = Target_ctl.var t "xp" and cents = Target_ctl.var t "cents" in
      ignore (find "15");
      Target_ctl.set t "xp" "20";
      ignore (next "20");
      (match results () with
      | first :: _ as rows -> check (first.n = 1 && first.addr = xp.addr) "result #1 is not at xp's address 0x%Lx. results: %s" xp.addr (show_rows rows)
      | [] -> failf "no results after next 20");
      ignore (moth [ "keep"; "#1"; "xp" ]);
      ignore (find "5.32");
      (match (page ~near:"xp" ()).rows with
      | first :: _ as rows ->
          check (first.addr = cents.addr && first.away = Some 16) "the first row near xp is not cents (0x%Lx) away 16. results: %s" cents.addr (show_rows rows)
      | [] -> failf "no rows near xp");
      ignore (moth [ "set"; "#1"; "99.99" ]);
      expect t "cents" "9999";
      ignore (moth [ "set"; "xp"; "95" ]);
      expect t "xp" "95";
      let k = kept_one "xp" in
      check (addr_of k = xp.addr && to_string (member "shown" k) = "95") "kept shows xp as %s, expected 95 at 0x%Lx" (show k) xp.addr;
      ignore (moth [ "undo" ]);
      expect t "xp" "20";
      ignore (moth [ "undo" ]);
      expect t "cents" "532";
      expect t "xp" "20")

(* 21: an address kept by the number it shows, and one that shows another number *)
let keep_address () =
  with_target (fun t ->
      let level = Target_ctl.var t "level" in
      let at = hex level.addr in
      ignore (moth [ "keep"; at; "2"; "lvl" ]);
      let k = kept_one "lvl" in
      check (addr_of k = level.addr && to_string (member "tag" k) = "whole number") "kept shows lvl as %s, expected a whole number at %s" (show k) at;
      let r = moth_raw [ "keep"; at; "7" ] in
      let said = String.trim r.err and want = sprintf "nothing at %s reads as 7." at in
      check (r.code = 1 && said = want) "keep %s 7 gave exit %d and \"%s\", expected exit 1 and \"%s\"" at r.code said want)

(* 22: results a page at a time; a row of the page shown last can be set *)
let pages () =
  with_target ~filler_small_mb:1 (fun _ ->
      ignore (find "3");
      let first = page ~n:5 () in
      check (List.length first.rows = 5 && first.total > 5) "results -n 5 gave %d rows of %d: %s" (List.length first.rows) first.total (show_rows first.rows);
      let second = page ~from:6 ~n:5 () in
      check (List.map (fun r -> r.n) second.rows = [ 6; 7; 8; 9; 10 ]) "results --from 6 -n 5 gave %s" (show_rows second.rows);
      let shared = List.filter (fun r -> List.exists (fun q -> q.addr = r.addr) first.rows) second.rows in
      check (shared = []) "results --from 6 -n 5 repeats addresses of the first page: %s" (show_rows shared);
      let said = to_string (member "message" (moth [ "set"; "#7"; "4" ])) in
      let row7 = List.find (fun r -> r.n = 7) second.rows in
      let want = Printf.sprintf "set #7 (0x%Lx) to 4 (was " row7.addr in
      check (String.starts_with ~prefix:want said) "set #7 4 said %S, not %S..." said want;
      (* row 7 is wherever the fewest-first order puts it, often a float of wine's own: put it back *)
      ignore (moth [ "undo" ]))

(* 23: a budget far too small for the hunt (about 15 MiB of candidates): groups go untracked, and
   the value is still found. 3 MiB, not 1: the first group (i32, 2 million matches in the 64 MiB
   filler) alone takes 2 MiB as a bitmap, and the helper untracks from the end of the list up to
   and including the first group, so under 1 MiB nothing stays tracked and, 2 being as common as
   3 there, no later number can narrow the hunt. 3 MiB keeps i32 and f32 and untracks the rest. *)
let over_budget () =
  with_daemon_env
    [ ("MOTH_BUDGET_MB", "3") ]
    (fun () ->
      with_target ~filler_small_mb:64 (fun t ->
          let f = find "3" in
          check (List.mem "untracked" (notes f)) "find 3 has no untracked note: %s" (show f);
          Target_ctl.set t "lives" "2";
          ignore (next "2");
          let lives = Target_ctl.var t "lives" and rows = results () in
          check (List.exists (fun r -> r.addr = lives.addr) rows) "no result at lives' address 0x%Lx. results: %s" lives.addr (show_rows rows)))

(* 24: status notices the game has gone and says which *)
let closed_status () =
  with_target (fun t ->
      let attached = member "attached" (moth [ "status" ]) in
      let name = match attached with `Null -> failf "status shows nothing attached" | a -> to_string (member "name" a) in
      Target_ctl.quit t;
      let s = moth [ "status" ] in
      check (member "attached" s = `Null && member "closed" s = `String name) "status after %s closed: %s" name (show s))

(* 25: stamina, a decimal whose number is never typed, found by how it moved alone, then written *)
let no_number () =
  with_target (fun t ->
      let stamina = Target_ctl.var t "stamina" in
      let o = moth [ "snapshot" ] in
      check (List.mem "snapshot" (notes o)) "snapshot has no snapshot note: %s" (show o);
      let early = moth_raw [ "results" ] in
      let said = String.trim early.err and want = "nothing to list yet. tell moth how the value moved first: moth next down, up, same or changed." in
      check (early.code = 1 && said = want) "results before any step gave exit %d and \"%s\", expected exit 1 and \"%s\"" early.code said want;
      Target_ctl.set t "stamina" "50";
      ignore (next "down");
      Target_ctl.set t "stamina" "55";
      ignore (next "up");
      ignore (next "same");
      let r = need_row (page ~all:true ~n:200 ()).rows stamina "decimal" in
      set_row r "99";
      expect t "stamina" "99")

(* 26: the first step says nothing moved, which keeps nearly everything; the steps after it,
   by how stamina moved, find it *)
let same_first () =
  with_target (fun t ->
      let stamina = Target_ctl.var t "stamina" in
      ignore (moth [ "snapshot" ]);
      ignore (next "same");
      Target_ctl.set t "stamina" "40";
      ignore (next "down");
      Target_ctl.set t "stamina" "45";
      ignore (next "up");
      let rows = (page ~all:true ~n:200 ()).rows in
      check (List.exists (fun r -> r.addr = stamina.addr) rows) "stamina (0x%Lx) is not listed. results: %s" stamina.addr (show_rows rows))

(* 27: a snapshot of a 1 GB target and the first step over it, by the helper's own times. At this
   size the groups still lean on the snapshot after that step, so a step by how stamina moved then
   compares with the snapshot as the first step left it, through the real helper's reads. *)
let snapshot_speed () =
  with_target ~filler_mb:1024 (fun t ->
      let stamina = Target_ctl.var t "stamina" in
      let rate what o =
        let ms = to_number (member "ms" o) and gb = to_number (member "bytes" o) /. gib in
        check (gb >= 1.) "%s read only %.3f GB, less than the 1 GB filler" what gb;
        let s_per_gb = ms /. 1000. /. gb in
        Printf.printf "%s: %.2f s/GB (%.2f GB in %.0f ms)\n%!" what s_per_gb gb ms;
        s_per_gb
      in
      let snap = rate "snapshot" (moth [ "snapshot" ]) in
      let same_step = next "same" in
      let same = rate "next same" same_step in
      check (snap < max_s_per_gb) "the snapshot took %.2f s/GB, over the %.1f s/GB bar" snap max_s_per_gb;
      check (same < max_s_per_gb) "next same took %.2f s/GB, over the %.1f s/GB bar" same max_s_per_gb;
      (* over 64,000,000 places, so the values it compares with are the snapshot's *)
      let i32 = match List.find_opt (fun g -> to_string (member "id" g) = "i32") (to_list (member "groups" same_step)) with Some g -> g | None -> failf "no i32 group in %s" (show same_step) in
      check
        (to_bool (member "has_values" i32) && to_int (member "count" i32) > 64_000_000)
        "after next same the i32 group is %s, expected over 64,000,000 places that still have values" (show i32);
      Target_ctl.set t "stamina" "40";
      let down = next "down" in
      check (not (to_bool (member "dead_end" down))) "next down after stamina went from 62.5 to 40 was a dead end: %s" (show down);
      match (page ~near:(hex stamina.addr) ()).rows with
      | first :: _ as rows ->
          check (first.addr = stamina.addr && first.away = Some 0) "the first row near stamina (0x%Lx) is not stamina's own, away 0. results: %s" stamina.addr (show_rows rows)
      | [] -> failf "no rows near stamina")

(* 28: a snapshot bigger than MOTH_SNAPSHOT_MB is refused, and the hunt there was goes on *)
let snapshot_over_limit () =
  with_daemon_env
    [ ("MOTH_SNAPSHOT_MB", "16") ]
    (fun () ->
      with_target ~filler_mb:64 (fun _ ->
          let f = find "3" in
          let r = moth_raw [ "snapshot" ] in
          let said = String.trim r.err in
          check
            (r.code = 1 && String.starts_with ~prefix:"the game has" said && contains said "more than moth's limit of 16 mb")
            "snapshot gave exit %d and \"%s\", expected exit 1 and the game having more than moth's limit of 16 mb" r.code said;
          let hunt = member "hunt" (moth [ "status" ]) in
          check (hunt <> `Null && counts hunt = counts f) "status after the refused snapshot is not the find 3 hunt. find: %s. status: %s" (show f) (show hunt)))

(* 29: 64 MiB of floats of 0.5, all within 1 as a decimal reads it. They keep their values, so
   at 0 the ones that never moved go at once, and the next number is quick *)
let zero_and_one () =
  with_target ~filler_half_mb:64 (fun t ->
      let points = Target_ctl.var t "points" in
      let f = find "1" in
      let f32 = match List.find_opt (fun g -> to_string (member "id" g) = "f32") (to_list (member "groups" f)) with Some g -> g | None -> failf "no f32 group in %s" (show f) in
      check
        (to_int (member "count" f32) > 16_000_000 && to_bool (member "has_values" f32))
        "find 1's f32 group is %s, expected over 16,000,000 matches with their values" (show f32);
      Target_ctl.set t "points" "0";
      let t0 = now () in
      let o = next "0" in
      let took = now () -. t0 in
      Printf.printf "next 0: %.0f ms in the helper, %.0f ms end to end\n%!" (to_number (member "ms" o)) (took *. 1000.);
      check (took < 3.) "next 0 took %.1f s end to end, expected under 3" took;
      let c = group_count o "f32" in
      check (c < 1000) "next 0 left %d f32 matches, expected under 1,000" c;
      match (page ~near:(hex points.addr) ()).rows with
      | first :: _ as rows -> check (first.addr = points.addr && first.away = Some 0) "the first row near points (0x%Lx) is not points' own, away 0. results: %s" points.addr (show_rows rows)
      | [] -> failf "no rows near points")

(* 30: a kept value the game works out again: the set does not hold, and kept says the game put
   it back *)
let put_back () =
  with_target (fun t ->
      let total = Target_ctl.var t "total" in
      ignore (moth [ "keep"; hex total.addr; "6"; "total" ]);
      let o = moth [ "set"; "total"; "9" ] in
      check (member "held" o = `Bool false) "set total 9 did not say held false: %s" (show o);
      await t "total" "6";
      let k = kept_one "total" in
      check (member "set_to" k = `String "9" && member "put_back" k = `Bool true) "kept shows total as %s, expected set_to \"9\" and put_back true" (show k))

(* ---- the daemon's own socket, for what the command line never sends ---- *)

type conn = { fd : Unix.file_descr; got : Buffer.t }

let connect_core () =
  let path = match Moth_core.Daemon.socket_path ~home:!home with Ok p -> p | Error m -> failwith m in
  let fd = Unix.socket ~cloexec:true Unix.PF_UNIX Unix.SOCK_STREAM 0 in
  match Unix.connect fd (Unix.ADDR_UNIX path) with
  | () -> { fd; got = Buffer.create 4096 }
  | exception Unix.Unix_error (e, _, _) ->
      Unix.close fd;
      failf "couldn't reach moth's daemon at %s (%s)" path (Unix.error_message e)

let hang_up c = try Unix.close c.fd with Unix.Unix_error _ -> ()

(* one request; [record] false asks it to leave no trace. A broken connection is an error, not a
   SIGPIPE that ends the harness *)
let request ?(record = true) c cmd =
  let j =
    `Assoc
      ([ ("id", `Int 1); ("proto", `Int Moth_core.Daemon.protocol); ("actor", `String "user"); ("cmd", `String cmd); ("args", `List []); ("opts", `Assoc []) ]
      @ if record then [] else [ ("record", `Bool false) ])
  in
  let s = Yojson.Safe.to_string j ^ "\n" in
  let pipe = Sys.signal Sys.sigpipe Sys.Signal_ignore in
  Fun.protect
    ~finally:(fun () -> Sys.set_signal Sys.sigpipe pipe)
    (fun () -> try ignore (restart (fun () -> Unix.write_substring c.fd s 0 (String.length s))) with Unix.Unix_error (e, _, _) -> failf "couldn't send %s (%s)" cmd (Unix.error_message e))

(* the next line the daemon writes, as json; None when none comes within [timeout] *)
let line_from c ~timeout =
  let deadline = now () +. timeout and chunk = Bytes.create 4096 in
  let rec go () =
    let s = Buffer.contents c.got in
    match String.index_opt s '\n' with
    | Some i ->
        Buffer.clear c.got;
        Buffer.add_string c.got (String.sub s (i + 1) (String.length s - i - 1));
        Some (Yojson.Safe.from_string (String.sub s 0 i))
    | None -> (
        let left = deadline -. now () in
        if left <= 0. then None
        else
          match restart (fun () -> Unix.select [ c.fd ] [] [] left) with
          | [], _, _ -> go ()
          | _ -> (
              match restart (fun () -> Unix.read c.fd chunk 0 (Bytes.length chunk)) with
              | 0 -> failwith "moth's daemon closed the connection"
              | n ->
                  Buffer.add_subbytes c.got chunk 0 n;
                  go ()))
  in
  go ()

(* the reply to the request just sent, which must say ok *)
let answered c what =
  match line_from c ~timeout:60. with
  | Some r when member "ok" r = `Bool true -> r
  | Some r -> failf "%s was refused: %s" what (show r)
  | None -> failf "%s got no answer within 60s" what

(* 31: a second client watches while the first works: the find as it starts and as it ends, its
   event the newest in the log; then a read that leaves no trace, which the watcher never hears of *)
let the_feed () =
  with_target (fun _ ->
      let w = connect_core () in
      Fun.protect
        ~finally:(fun () -> hang_up w)
        (fun () ->
          request w "watch";
          ignore (answered w "watch");
          ignore (find "3");
          let heard what = match line_from w ~timeout:10. with Some l -> l | None -> failf "the watcher heard no %s within 10s" what in
          (* a feed line [kind] (busy or event) for [cmd] *)
          let is kind cmd l = match member kind l with `Assoc _ as o -> member "cmd" o = `String cmd | _ -> false in
          let busy = heard "start of find 3" in
          check (is "busy" "find" busy && member "args" (member "busy" busy) = `List [ `String "3" ]) "the watcher's first line after find 3 is %s, expected find's busy line" (show busy);
          let ev = heard "event for find 3" in
          check (is "event" "find" ev) "the watcher's second line after find 3 is %s, expected find's event" (show ev);
          let seq = to_int (member "seq" (member "event" ev)) in
          (match to_list (moth [ "log"; "-n"; "1" ]) with
          | [ newest ] -> check (member "seq" newest = `Int seq) "moth log's newest event is %s; the watcher's find event has seq %d" (show newest) seq
          | l -> failf "moth log -n 1 gave %s" (show (`List l)));
          (* moth log is recorded too: the watcher hears it start and end *)
          List.iter
            (fun kind ->
              let l = heard ("log's " ^ kind ^ " line") in
              check (is kind "log" l) "the watcher heard %s, expected log's %s line" (show l) kind)
            [ "busy"; "event" ];
          let c = connect_core () in
          Fun.protect ~finally:(fun () -> hang_up c) (fun () ->
              request ~record:false c "status";
              ignore (answered c "status with record false"));
          match line_from w ~timeout:1. with None -> () | Some l -> failf "after a status with record false the watcher heard %s" (show l)))

(* waits until moth's helper no longer lists [name] in the test bottle *)
let gone_from_bottle name =
  let deadline = now () +. 30. in
  let rec go () =
    let procs = to_list (moth [ "ps"; "--bottle"; bottle () ]) in
    if not (List.exists (fun p -> String.lowercase_ascii (to_string (member "name" p)) = name) procs) then ()
    else if now () > deadline then failf "%s is still running in %s 30s after it was told to quit: %s" name (bottle ()) (show (`List procs))
    else (Unix.sleepf 0.5; go ())
  in
  go ()

(* 32: moth launches the stand-in game, which connects back as when the harness starts it; games
   remembers it, attach finds it by name, and it is told to quit *)
let launched () =
  let path = exe "moth-target.exe" in
  if not (Sys.file_exists path) then failf "%s not found. run make helper." path;
  let t = Target_ctl.adopt (fun port -> ignore (moth [ "launch"; "--bottle"; bottle (); path; string_of_int port ])) in
  Fun.protect
    ~finally:(fun () -> Target_ctl.stop t)
    (fun () ->
      let games = to_list (moth [ "games" ]) in
      let this g = member "name" g = `String "moth-target.exe" && member "bottle" g = `String (bottle ()) && member "path" g = `String path in
      check (List.exists this games) "games does not list moth-target.exe in %s at %s: %s" (bottle ()) path (show (`List games));
      ignore (moth [ "attach"; "--bottle"; bottle (); "moth-target.exe" ]);
      let attached = member "attached" (moth [ "status" ]) in
      check (attached <> `Null && String.lowercase_ascii (to_string (member "name" attached)) = "moth-target.exe") "status after attach shows %s, not moth-target.exe" (show attached);
      Target_ctl.quit t;
      gone_from_bottle "moth-target.exe")

(* ---- the window: build/app/it_app drives the real one, offscreen, against this home ---- *)

let it_app () = Filename.concat (Filename.dirname !bin) "build/app/it_app"

(* Runs it_app with [args] against this home's daemon, and hands [on_line] each line it prints as
   it prints it. Its stderr (Qt's own notes) goes to it_app.log in the home. It must end with ok
   and exit 0. It is killed when it runs past [timeout] (its own limit is 180s) or when [on_line]
   fails. *)
let app_pid = ref 0

(* with [~stdin:true], what [on_line] can say to the running it_app, a line at a time *)
let app_say : (string -> unit) ref = ref (fun _ -> failwith "it_app was started without ~stdin")

let app_ok ?(on_line = fun _ -> ()) ?(timeout = 240.) ?(vars = []) ?(stdin = false) args =
  let prog = it_app () in
  if not (Sys.file_exists prog) then failf "%s not found. run make app." prog;
  let env = env_with ([ ("MOTH_BIN", exe "moth"); ("QT_QPA_PLATFORM", "offscreen") ] @ vars) in
  let out_r, out_w = Unix.pipe ~cloexec:true () in
  let null, in_w =
    if stdin then
      let r, w = Unix.pipe ~cloexec:true () in
      (r, Some w)
    else (Unix.openfile "/dev/null" [ Unix.O_RDONLY; Unix.O_CLOEXEC ] 0, None)
  in
  let err = Unix.openfile (Filename.concat !home "it_app.log") [ Unix.O_WRONLY; Unix.O_CREAT; Unix.O_APPEND; Unix.O_CLOEXEC ] 0o644 in
  let pid =
    Fun.protect ~finally:(fun () -> List.iter Unix.close [ null; out_w; err ]) (fun () -> Unix.create_process_env prog (Array.of_list (prog :: args)) env null out_w err)
  in
  app_pid := pid;
  let close_in () = Option.iter (fun w -> try Unix.close w with Unix.Unix_error _ -> ()) in_w in
  app_say :=
    (fun line ->
      let text = line ^ "\n" in
      match in_w with
      | Some w -> (
          match Unix.write_substring w text 0 (String.length text) with
          | n when n = String.length text -> ()
          | _ -> failf "could not tell it_app %s" line
          | exception Unix.Unix_error (e, _, _) -> failf "could not tell it_app %s: %s" line (Unix.error_message e))
      | None -> failf "it_app was started without ~stdin");
  let lines = ref [] and partial = Buffer.create 256 and chunk = Bytes.create 4096 in
  let rec take () =
    let s = Buffer.contents partial in
    match String.index_opt s '\n' with
    | None -> ()
    | Some i ->
        let line = String.sub s 0 i in
        Buffer.clear partial;
        Buffer.add_string partial (String.sub s (i + 1) (String.length s - i - 1));
        if !verbose then Printf.eprintf "it_app: %s\n%!" line;
        lines := line :: !lines;
        on_line line;
        take ()
  in
  let deadline = now () +. timeout in
  let rec pump () =
    let left = deadline -. now () in
    left > 0.
    &&
    match restart (fun () -> Unix.select [ out_r ] [] [] left) with
    | [], _, _ -> pump ()
    | _ -> (
        match restart (fun () -> Unix.read out_r chunk 0 (Bytes.length chunk)) with
        | 0 -> true
        | n ->
            Buffer.add_subbytes partial chunk 0 n;
            take ();
            pump ())
  in
  let reap () = match snd (restart (fun () -> Unix.waitpid [] pid)) with Unix.WEXITED c -> c | WSIGNALED s | WSTOPPED s -> 128 + abs s in
  let stop () =
    (try Unix.kill pid Sys.sigkill with Unix.Unix_error _ -> ());
    ignore (reap ())
  in
  let finished = Fun.protect ~finally:(fun () -> Unix.close out_r; close_in ()) (fun () -> try pump () with e -> stop (); raise e) in
  let printed () = String.concat " | " (List.rev !lines) in
  if not finished then (
    stop ();
    failf "it_app %s did not finish within %.0fs. it printed: %s" (String.concat " " args) timeout (printed ()));
  let code = reap () in
  check (code = 0 && List.nth_opt !lines 0 = Some "ok") "it_app %s ended with exit %d. it printed: %s" (String.concat " " args) code (printed ())

(* 33: the window against the real game: attached from the game bar, 3 typed and Return, the row
   at points' address typed over with 9 and kept; the game then has 9 *)
let window () =
  with_target ~attach:false (fun t ->
      Target_ctl.set t "points" "3";
      let points = Target_ctl.var t "points" in
      app_ok [ "edit"; bottle (); "moth-target.exe"; "3"; hex points.addr; "9" ];
      expect t "points" "9")

(* 34: the window lists a hunt; once it shows points' row, the game moves points to 4 and moth
   next 4 runs from the command line. The list reads itself again and shows 4 there, with
   nothing clicked *)
let list_follows () =
  with_target ~attach:false (fun t ->
      Target_ctl.set t "points" "3";
      let points = Target_ctl.var t "points" in
      let narrowed = ref false in
      let on_line = function
        | "listed" ->
            Target_ctl.set t "points" "4";
            ignore (moth ~timeout:60. [ "next"; "4" ]);
            narrowed := true
        | _ -> ()
      in
      app_ok ~on_line [ "follow"; bottle (); "moth-target.exe"; "3"; hex points.addr; "4" ];
      check !narrowed "it_app never printed listed")

(* 35: two hands on rows. While the window shows a list and polls it, the command line lists near
   points and writes its row 1: the window's own reads must not change which row that is, so the
   write lands on points, and the window's list shows it *)
let two_hands () =
  with_target ~attach:false (fun t ->
      Target_ctl.set t "points" "3";
      let points = Target_ctl.var t "points" in
      let wrote = ref false in
      let on_line = function
        | "listed" ->
            Unix.sleepf 2.;
            let near = (page ~near:(hex points.addr) ()).rows in
            (match near with
            | r :: _ -> check (r.n = 1 && r.addr = points.addr) "row 1 near points is %s, not at points' address 0x%Lx" (show_rows [ r ]) points.addr
            | [] -> failf "nothing listed near points' address 0x%Lx" points.addr);
            Unix.sleepf 2.;
            let said = to_string (member "message" (moth ~timeout:60. [ "set"; "1"; "4" ])) in
            check (String.starts_with ~prefix:(Printf.sprintf "set #1 (0x%Lx) to 4" points.addr) said) "moth set 1 4 said: %s" said;
            expect t "points" "4";
            wrote := true
        | _ -> ()
      in
      app_ok ~on_line [ "follow"; bottle (); "moth-target.exe"; "3"; hex points.addr; "4" ];
      check !wrote "it_app never printed listed")

(* 36: the game goes away while the window shows its hunt: the window says so in the core's words,
   lists and keeps nothing, and a find from the search bar is refused; nothing writes *)
let game_goes () =
  with_target ~attach:false (fun t ->
      Target_ctl.set t "points" "3";
      let points = Target_ctl.var t "points" in
      let events () = to_list (moth [ "log"; "-n"; "1000" ]) in
      let before = match events () with ev :: _ -> to_int (member "seq" ev) | [] -> 0 in
      let quit = ref false in
      let on_line = function
        | "listed" ->
            Target_ctl.quit t;
            quit := true
        | _ -> ()
      in
      app_ok ~on_line [ "gone"; bottle (); "moth-target.exe"; "3"; hex points.addr; "0" ];
      check !quit "it_app never printed listed";
      let since = List.filter (fun ev -> to_int (member "seq" ev) > before) (events ()) in
      let writes = List.filter (fun ev -> List.mem (member "cmd" ev) [ `String "set"; `String "keep"; `String "undo" ]) since in
      check (writes = []) "something wrote: %s" (show (`List writes)))

(* ---- an agent's hand, and the agent in the window (37 to 42) ---- *)

let read_text path = try In_channel.with_open_bin path In_channel.input_all with Sys_error _ -> ""
let lines_of text = List.filter (fun l -> l <> "") (String.split_on_char '\n' text)

(* the stand-in for claude code, built next to this program *)
let stub_agent () =
  let dir = Filename.dirname Sys.executable_name in
  let dir = if Filename.is_relative dir then Filename.concat (Sys.getcwd ()) dir else dir in
  let p = Filename.concat dir "stub_agent.exe" in
  if not (Sys.file_exists p) then failf "%s not found. run make build." p;
  p

let mkdir_p dir = List.iter (fun d -> try Unix.mkdir d 0o700 with Unix.Unix_error (Unix.EEXIST, _, _) -> ()) (let rec up d = if d = "/" || d = "." then [] else up (Filename.dirname d) @ [ d ] in up dir)

(* waits up to [timeout] seconds for [f] to be true *)
let until ~timeout f =
  let deadline = now () +. timeout in
  let rec go () = f () || (now () < deadline && (Unix.sleepf 0.1; go ())) in
  go ()

(* every process below [root], by ps *)
let descendants root =
  let r = run ~timeout:20. [| "/bin/ps"; "-axo"; "pid=,ppid=" |] in
  let pairs =
    List.filter_map
      (fun l -> match List.filter (( <> ) "") (String.split_on_char ' ' (String.trim l)) with [ a; b ] -> (match (int_of_string_opt a, int_of_string_opt b) with Some a, Some b -> Some (a, b) | _ -> None) | _ -> None)
      (String.split_on_char '\n' r.out)
  in
  let rec below acc = function
    | [] -> acc
    | p :: rest ->
        let kids = List.filter_map (fun (c, par) -> if par = p && not (List.mem c acc) then Some c else None) pairs in
        below (acc @ kids) (rest @ kids)
  in
  below [] [ root ]

(* the processes a window started that the harness did not: tracked while it runs, and checked gone after *)
let tracked = ref []
let track_app () =
  tracked := List.sort_uniq compare (descendants !app_pid @ !tracked);
  if !verbose then Printf.eprintf "processes below it_app: %s\n%!" (String.concat " " (List.map string_of_int !tracked))

let rec rm p =
  match Unix.lstat p with
  | exception Unix.Unix_error _ -> ()
  | { st_kind = S_DIR; _ } ->
      Array.iter (fun f -> rm (Filename.concat p f)) (Sys.readdir p);
      Unix.rmdir p
  | _ -> Sys.remove p

(* what is still running of what a window started: the tracked pids and anything in ps started
   with this run's home on its command line *)
let leftovers () =
  let r = run ~timeout:20. [| "/bin/ps"; "-axo"; "pid=,args=" |] in
  let by_args =
    List.filter_map
      (fun l ->
        let l = String.trim l in
        if contains l !home && (contains l "stub_agent" || contains l "moth wait" || contains l "moth-app") then
          int_of_string_opt (List.hd (String.split_on_char ' ' l))
        else None)
      (String.split_on_char '\n' r.out)
  in
  List.sort_uniq compare (List.filter alive !tracked @ by_args)

(* runs [f], then checks nothing it started is left. When [f] fails, whatever is left is killed
   first and the failure goes on, so a failed scenario leaves no stand-in or waiter behind *)
let with_nothing_left what f =
  (match f () with
  | () -> ()
  | exception e ->
      List.iter (fun p -> try Unix.kill p Sys.sigkill with Unix.Unix_error _ -> ()) (leftovers ());
      tracked := [];
      raise e);
  let left = List.filter (fun p -> not (wait_gone p ~timeout:10.)) !tracked in
  let more = leftovers () in
  tracked := [];
  List.iter (fun p -> try Unix.kill p Sys.sigkill with Unix.Unix_error _ -> ()) (left @ more);
  check (left @ more = []) "after %s these processes were still running (killed): %s" what (String.concat ", " (List.map string_of_int (left @ more)))

(* [f] with a home whose name has a space in it (and the service of it): the settings file, the
   hook's command line and the session folder all carry the space. The daemon of the usual home is
   stopped first and the next scenario starts one again. *)
let with_spaced_home f =
  let usual = !home in
  check (stop_daemon ()) "moth's daemon did not stop when asked";
  let spaced = sprintf "/tmp/moth it %d" (Unix.getpid ()) in
  rm spaced;
  Unix.mkdir spaced 0o700;
  home := spaced;
  Unix.putenv "MOTH_HOME" spaced;
  let back () =
    ignore (stop_daemon ());
    home := usual;
    Unix.putenv "MOTH_HOME" usual
  in
  match f () with
  | () ->
      back ();
      rm spaced
  | exception e ->
      back ();
      Printf.printf "logs of the spaced home kept in %s\n%!" spaced;
      raise e

(* a temporary HOME for the window's shell, with a space in its name *)
let fake_home () =
  let h = Filename.concat !home "fake home" in
  mkdir_p h;
  h

(* 37: what the agent runs is recorded as the agent's; an actor that is neither is refused *)
let agents_hand () =
  with_target (fun t ->
      Target_ctl.set t "points" "3";
      let agent_env = env_with [ ("MOTH_ACTOR", "agent") ] in
      ignore (moth ~env:agent_env [ "find"; "3" ]);
      let events = to_list (moth [ "log"; "-n"; "5" ]) in
      (match List.find_opt (fun ev -> member "cmd" ev = `String "find") events with
      | Some ev -> check (member "actor" ev = `String "agent") "the find was recorded as %s, expected actor agent" (show ev)
      | None -> failf "no find in the log: %s" (show (`List events)));
      ignore (moth [ "next"; "3" ]);
      (match List.find_opt (fun ev -> member "cmd" ev = `String "next") (to_list (moth [ "log"; "-n"; "5" ])) with
      | Some ev -> check (member "actor" ev = `String "user") "a next with no MOTH_ACTOR was recorded as %s, expected actor user" (show ev)
      | None -> failf "no next in the log");
      let r = moth_raw ~env:(env_with [ ("MOTH_ACTOR", "robot") ]) [ "status" ] in
      check (r.code = 2 && r.err = "MOTH_ACTOR must be user or agent\n") "MOTH_ACTOR=robot moth status gave exit %d, stderr %S" r.code r.err)

(* a moth wait of our own, its stderr in a file *)
type waiter = { wpid : int; werr : string; mutable ended : int option }

let start_waiter dir =
  let werr = Filename.concat !home (sprintf "waiter-%d.err" (int_of_float (now () *. 1000.))) in
  let err = Unix.openfile werr [ Unix.O_WRONLY; Unix.O_CREAT; Unix.O_TRUNC; Unix.O_CLOEXEC ] 0o600 in
  let null = Unix.openfile "/dev/null" [ Unix.O_RDWR; Unix.O_CLOEXEC ] 0 in
  let argv = [| exe "moth"; "wait"; "--session"; dir |] in
  let wpid = Fun.protect ~finally:(fun () -> Unix.close err; Unix.close null) (fun () -> Unix.create_process argv.(0) argv null null err) in
  { wpid; werr; ended = None }

let waiter_code w =
  (match w.ended with
  | None -> (
      match Unix.waitpid [ Unix.WNOHANG ] w.wpid with
      | 0, _ -> ()
      | _, st -> w.ended <- Some (match st with Unix.WEXITED c -> c | WSIGNALED s | WSTOPPED s -> 128 + abs s)
      | exception Unix.Unix_error _ -> w.ended <- Some 0)
  | Some _ -> ());
  w.ended

let stop_waiter w =
  if waiter_code w = None then (
    (try Unix.kill w.wpid Sys.sigkill with Unix.Unix_error _ -> ());
    ignore (try Unix.waitpid [] w.wpid with Unix.Unix_error _ -> (0, Unix.WEXITED 0)));
  check (not (alive w.wpid)) "moth wait (pid %d) is still running" w.wpid

(* the newest event of [cmd] in the log, its seq *)
let latest_seq cmd =
  match List.find_opt (fun ev -> member "cmd" ev = `String cmd) (to_list (moth [ "log"; "-n"; "20" ])) with
  | Some ev -> to_int (member "seq" ev)
  | None -> failf "no %s in the log" cmd

(* the cursor file's seq and whether it says quiet *)
let cursor_of dir =
  match String.split_on_char ' ' (String.trim (read_text (Filename.concat dir "cursor"))) with
  | [ n ] -> Option.map (fun n -> (n, false)) (int_of_string_opt n)
  | [ n; "quiet" ] -> Option.map (fun n -> (n, true)) (int_of_string_opt n)
  | _ -> None

(* 38: moth wait tells what the person did, in order, and keeps quiet about the agent's own steps
   and, while quiet is on, the person's. Run in a home with a space in its name. *)
let waits () =
  with_spaced_home (fun () ->
      with_target (fun t ->
          Target_ctl.set t "points" "3";
          ignore (moth [ "quiet"; "off" ]);
          let dir = Filename.concat (Filename.concat !home "sessions") (string_of_int (Unix.getpid ())) in
          mkdir_p dir;
          let waiters = ref [] in
          let start () = let w = start_waiter dir in waiters := w :: !waiters; w in
          Fun.protect
            ~finally:(fun () -> List.iter stop_waiter !waiters)
            (fun () ->
              let w1 = start () in
              check (until ~timeout:15. (fun () -> Sys.file_exists (Filename.concat dir "cursor"))) "moth wait wrote no cursor in 15s";
              ignore (find "3");
              Unix.sleepf 0.5;
              Target_ctl.set t "points" "4";
              ignore (next "4");
              check (until ~timeout:6. (fun () -> waiter_code w1 <> None)) "moth wait did not end within 6s of the person's find and next";
              let err1 = read_text w1.werr in
              check (waiter_code w1 = Some 2) "moth wait ended with %s, expected 2. stderr: %s" (match waiter_code w1 with Some c -> string_of_int c | None -> "?") err1;
              (match lines_of err1 with
              | [ head; a; b ] ->
                  check (head = "[moth] the person did this in moth:") "the note starts %S" head;
                  check (String.starts_with ~prefix:"- find 3: " a && String.starts_with ~prefix:"- next 4: " b) "the note's steps are %S and %S, expected find 3 then next 4" a b
              | l -> failf "the note is %d lines, expected a heading and two steps: %s" (List.length l) err1);
              (* the second waiter, shown to be watching before anything is tried on it: while quiet
                 it writes its cursor as it moves, so once the cursor file says quiet at the quiet
                 event's seq it has read the feed up to there *)
              let w2 = start () in
              ignore (moth [ "quiet"; "on" ]);
              let armed = latest_seq "quiet" in
              check (until ~timeout:15. (fun () -> match cursor_of dir with Some (n, true) -> n >= armed | _ -> false))
                "the second moth wait never wrote its cursor at quiet on (seq %d): %S" armed (read_text (Filename.concat dir "cursor"));
              check (waiter_code w2 = None) "the second moth wait ended (%s) over quiet on" (read_text w2.werr);
              ignore (moth [ "quiet"; "off" ]);
              (* the agent's own step *)
              Target_ctl.set t "points" "5";
              ignore (moth ~env:(env_with [ ("MOTH_ACTOR", "agent") ]) [ "next"; "5" ]);
              Unix.sleepf 3.;
              check (waiter_code w2 = None) "moth wait ended (%s) over the agent's own step" (read_text w2.werr);
              (* the person's, while quiet: the waiter must have read it (its cursor moves) and not told it *)
              ignore (moth [ "quiet"; "on" ]);
              ignore (next "5");
              let stepped = latest_seq "next" in
              check (until ~timeout:15. (fun () -> match cursor_of dir with Some (n, true) -> n >= stepped | _ -> false))
                "the second moth wait never read the person's step while quiet (seq %d): %S" stepped (read_text (Filename.concat dir "cursor"));
              Unix.sleepf 3.;
              check (waiter_code w2 = None) "moth wait ended (%s) over a step of the person's while quiet was on" (read_text w2.werr);
              ignore (moth [ "quiet"; "off" ]);
              Target_ctl.set t "points" "3";
              ignore (find "3");
              check (until ~timeout:6. (fun () -> waiter_code w2 <> None)) "moth wait did not end within 6s of the person's find after quiet off";
              let err2 = read_text w2.werr in
              check (waiter_code w2 = Some 2) "the second moth wait ended with %s, expected 2. stderr: %s"
                (match waiter_code w2 with Some c -> string_of_int c | None -> "?") err2;
              match lines_of err2 with
              | [ head; a ] -> check (head = "[moth] the person did this in moth:" && String.starts_with ~prefix:"- find 3: " a) "the second note is %S, expected the find alone" err2
              | _ -> failf "the second note is not a heading and one step: %s" err2)))

(* the stand-in's own files, in this scenario's home *)
let stub_file name = Filename.concat !home name

(* how many times the stand-in started its Stop hook: stub-runs has a line per start, its event *)
let stop_runs runs = List.length (List.filter (( = ) "Stop") (lines_of (read_text runs)))

let heading = "[moth] the person did this in moth:"

(* how many notes [text] holds: its headings *)
let notes_in text =
  let n = String.length heading in
  let rec go i k = if i + n > String.length text then k else if String.sub text i n = heading then go (i + n) (k + 1) else go (i + 1) k in
  go 0 0

(* the first note in stub-notes: from the first heading to the next, or to the end *)
let first_note text =
  let rec at i = if i + String.length heading > String.length text then None else if String.sub text i (String.length heading) = heading then Some i else at (i + 1) in
  match at 0 with
  | None -> ""
  | Some a ->
      let b = Option.value (at (a + String.length heading)) ~default:(String.length text) in
      String.sub text a (b - a)

(* 39: the window's terminal runs moth agent, which runs the stand-in; what the person does in the
   window reaches the stand-in as a note, quiet stops that (and the stand-in is shown alive and
   waiting while it does, and told again once quiet is off), and when the agent ends the terminal is
   a shell that can run moth. The stand-in's first turn starts 5 seconds after it is up
   (STUB_FIRST_DELAY), as Claude Code loads before its first prompt is submitted: the window's
   attach and find come before any waiter, and the first note must still tell the find. Each turn
   arms two waiters (a turn's start and its end), and one step must still bring one note. Run in a
   home with a space in its name. *)
let agent_in_window () =
  with_spaced_home (fun () ->
      with_target ~attach:false (fun t ->
          Target_ctl.set t "points" "3";
          ignore (moth [ "quiet"; "off" ]);
          let notes = stub_file "stub-notes" and args = stub_file "stub-args" and runs = stub_file "stub-runs" and envf = stub_file "stub-env" in
          List.iter (fun f -> try Sys.remove f with Sys_error _ -> ()) [ notes; args; runs; envf ];
          let vars = [ ("MOTH_CLAUDE", stub_agent ()); ("STUB_FIRST_DELAY", "5"); ("SHELL", "/bin/sh"); ("HOME", fake_home ()) ] in
          let settings_of a = List.find_opt (fun l -> String.starts_with ~prefix:(!home ^ "/agent/") l && String.ends_with ~suffix:"/settings.json" l) a in
          let after_notes = ref "" in
          let on_line = function
            | "clicked" ->
                track_app ();
                (* the find is done and no waiter has started yet *)
                check (not (Sys.file_exists runs)) "the stand-in ran its hook before the window's find was done (stub-runs: %S), so the find was not made before the first waiter" (read_text runs);
                check (until ~timeout:15. (fun () -> contains (read_text notes) "find 3")) "stub-notes has no note with find 3 within 15s. it holds: %S" (read_text notes);
                let first = first_note (read_text notes) in
                check (contains first "- find 3: ") "the first note does not tell the find made before the first waiter: %S" first;
                (* the hook armed again at the end of the turn the note started *)
                check (until ~timeout:10. (fun () -> stop_runs runs >= 2)) "the stand-in did not run the Stop hook again after the note (stub-runs: %S)" (read_text runs);
                let a = lines_of (read_text args) in
                check (List.mem "--settings" a) "the stand-in's arguments have no --settings: %s" (String.concat " | " a);
                check (settings_of a <> None) "no settings file under %s/agent/ among the arguments: %s" !home (String.concat " | " a);
                (* the settings' env, as the stand-in ran the hook with it *)
                let env = lines_of (read_text envf) in
                check (List.mem "MOTH_ACTOR=agent" env) "the settings' env has no MOTH_ACTOR=agent: %s" (String.concat " " env);
                check (List.mem ("MOTH_HOME=" ^ !home) env) "the settings' env has no MOTH_HOME=%s: %s" !home (String.concat " " env);
                check (List.exists (String.starts_with ~prefix:("PATH=" ^ !bin ^ ":")) env) "the settings' PATH does not start with %s: %s" !bin (String.concat " " env);
                !app_say "quiet"
            | "quiet" ->
                track_app ();
                check (member "quiet" (moth [ "status" ]) = `Bool true) "status after the box was ticked does not say quiet";
                let before = read_text notes in
                Target_ctl.set t "points" "4";
                ignore (next "4");
                Unix.sleepf 4.;
                check (read_text notes = before) "a step of the person's while quiet was on reached the agent: %S" (read_text notes);
                (* the silence was the switch: the stand-in is alive and a moth wait is under it *)
                let session = match settings_of (lines_of (read_text args)) with Some p -> int_of_string (Filename.basename (Filename.dirname p)) | None -> failf "no settings path" in
                check (alive session) "the stand-in (pid %d) is gone after the quiet step" session;
                let below = descendants session in
                let waiter = match int_of_string_opt (String.trim (read_text (Filename.concat (Filename.concat (Filename.concat !home "agent") (string_of_int session)) "waiter"))) with Some p -> p | None -> failf "no waiter file" in
                check (alive waiter && List.mem waiter below) "no live moth wait (pid %d) under the stand-in (pid %d; below it: %s)" waiter session (String.concat " " (List.map string_of_int below));
                after_notes := before;
                !app_say "loud"
            | "loud" ->
                track_app ();
                check (member "quiet" (moth [ "status" ]) = `Bool false) "status after the box was unticked does not say loud";
                Target_ctl.set t "points" "3";
                ignore (find "3");
                check (until ~timeout:10. (fun () -> String.length (read_text notes) > String.length !after_notes)) "no note reached the agent within 10s of a step once quiet was off";
                (* a second note of the same step, from the turn's other waiter, would have come by now *)
                Unix.sleepf 2.;
                let fresh = String.sub (read_text notes) (String.length !after_notes) (String.length (read_text notes) - String.length !after_notes) in
                check (notes_in fresh = 1 && contains fresh "find 3" && not (contains fresh "next 4")) "the note after quiet off is not one note with the find alone: %S" fresh;
                !app_say "bye"
            | _ -> ()
          in
          with_nothing_left "the window closed" (fun () -> app_ok ~stdin:true ~vars ~on_line [ "term"; bottle (); "moth-target.exe"; "3"; "stub agent up" ]);
          check (member "quiet" (moth [ "status" ]) = `Bool false) "quiet is on after the scenario"))

(* 40: no claude code: the terminal says so, is a shell, and the panels still work *)
let no_agent () =
  with_target ~attach:false (fun t ->
      Target_ctl.set t "points" "3";
      ignore (moth [ "status" ]);
      let vars = [ ("MOTH_CLAUDE", "/nonexistent"); ("SHELL", "/bin/sh"); ("HOME", fake_home ()) ] in
      let on_line = function
        | "clicked" ->
            track_app ();
            !app_say "shell"
        | _ -> ()
      in
      with_nothing_left "the window closed" (fun () -> app_ok ~stdin:true ~vars ~on_line [ "term"; bottle (); "moth-target.exe"; "3"; "claude code is not installed" ]))

(* the stand-in's pid: its session folder's name, from the settings path in its arguments *)
let session_of args = match List.find_opt (fun l -> String.starts_with ~prefix:(!home ^ "/agent/") l && String.ends_with ~suffix:"/settings.json" l) args with
  | Some p -> int_of_string (Filename.basename (Filename.dirname p))
  | None -> failf "no settings file under %s/agent/ among the stand-in's arguments: %s" !home (String.concat " | " args)

(* 41: the window closes while the agent is up and a waiter is armed under it, and the agent
   ignores the hang-up and never reads its terminal (STUB_DEAF): closing the window still ends it,
   and its moth wait with it *)
let window_goes_with_agent () =
  with_target ~attach:false (fun t ->
      Target_ctl.set t "points" "3";
      ignore (moth [ "quiet"; "off" ]);
      let notes = stub_file "stub-notes" and args = stub_file "stub-args" and runs = stub_file "stub-runs" and envf = stub_file "stub-env" in
      List.iter (fun f -> try Sys.remove f with Sys_error _ -> ()) [ notes; args; runs; envf ];
      let vars = [ ("MOTH_CLAUDE", stub_agent ()); ("STUB_DEAF", "1"); ("SHELL", "/bin/sh"); ("HOME", fake_home ()) ] in
      let session = ref 0 and waiter = ref 0 in
      let on_line = function
        | "clicked" ->
            track_app ();
            check (until ~timeout:10. (fun () -> contains (read_text notes) "find 3")) "stub-notes has no note with find 3 within 10s. it holds: %S" (read_text notes);
            check (until ~timeout:10. (fun () -> stop_runs runs >= 2)) "the stand-in did not run the Stop hook again after the note (stub-runs: %S)" (read_text runs);
            session := session_of (lines_of (read_text args));
            let waiter_file = Filename.concat (Filename.concat (Filename.concat !home "agent") (string_of_int !session)) "waiter" in
            let armed () =
              match int_of_string_opt (String.trim (read_text waiter_file)) with
              | Some p when alive p && List.mem p (descendants !session) -> waiter := p; true
              | _ -> false
            in
            check (until ~timeout:10. armed) "no live moth wait under the stand-in (pid %d) within 10s (waiter file: %S)" !session (read_text waiter_file);
            track_app ();
            !app_say "close"
        | _ -> ()
      in
      with_nothing_left "the window closed with the agent up" (fun () -> app_ok ~stdin:true ~vars ~on_line [ "term"; bottle (); "moth-target.exe"; "3"; "stub agent up" ]);
      check (!session > 0 && not (alive !session) && not (alive !waiter)) "the stand-in (pid %d) or its moth wait (pid %d) outlived the window" !session !waiter)

(* the moth wait processes running for the session folder [dir], by ps (moth's own process: the
   hook's shell, when it is one, is not counted) *)
let waiters_for dir =
  let r = run ~timeout:20. [| "/bin/ps"; "-axo"; "pid=,args=" |] in
  List.filter_map
    (fun l ->
      let l = String.trim l in
      if String.ends_with ~suffix:("moth wait --session " ^ dir) l then int_of_string_opt (List.hd (String.split_on_char ' ' l)) else None)
    (String.split_on_char '\n' r.out)

(* 42: every turn of the stand-in is interrupted (STUB_INTERRUPT), as when the person presses
   Escape, so no Stop hook runs: the waiter armed as each turn starts still tells the window's
   find, then a later step of the person's in a note of its own, and exactly one moth wait is left
   for the session *)
let interrupted_turn () =
  with_target ~attach:false (fun t ->
      Target_ctl.set t "points" "3";
      ignore (moth [ "quiet"; "off" ]);
      let notes = stub_file "stub-notes" and args = stub_file "stub-args" and runs = stub_file "stub-runs" and envf = stub_file "stub-env" in
      List.iter (fun f -> try Sys.remove f with Sys_error _ -> ()) [ notes; args; runs; envf ];
      let vars = [ ("MOTH_CLAUDE", stub_agent ()); ("STUB_INTERRUPT", "1"); ("SHELL", "/bin/sh"); ("HOME", fake_home ()) ] in
      let on_line = function
        | "clicked" ->
            track_app ();
            (* each check's words are read after its wait *)
            let told = until ~timeout:10. (fun () -> contains (read_text notes) "- find 3: ") in
            check told "stub-notes has no note with find 3 within 10s. it holds: %S" (read_text notes);
            let session = session_of (lines_of (read_text args)) in
            let before = read_text notes in
            Target_ctl.set t "points" "4";
            ignore (next "4");
            let told = until ~timeout:10. (fun () -> contains (read_text notes) "- next 4: ") in
            check told "no note with next 4 within 10s of the person's next 4. stub-notes holds: %S" (read_text notes);
            (* a second note of the same step, from a second waiter, would have come by now *)
            Unix.sleepf 2.;
            let text = read_text notes in
            let fresh = String.sub text (String.length before) (String.length text - String.length before) in
            check (notes_in fresh = 1 && not (contains fresh "find 3")) "the person's next 4 did not bring one note with that step alone: %S" fresh;
            check (stop_runs runs = 0) "the stand-in ran a Stop hook in an interrupted turn (stub-runs: %S)" (read_text runs);
            let dir = Filename.concat (Filename.concat !home "agent") (string_of_int session) in
            let one = until ~timeout:5. (fun () -> List.length (waiters_for dir) = 1) in
            let ws = waiters_for dir in
            check (one && List.length ws = 1) "%d moth wait for the session (pids %s), expected exactly one" (List.length ws) (String.concat " " (List.map string_of_int ws));
            let w = List.hd ws in
            check (List.mem w (descendants session)) "the one moth wait (pid %d) is not under the stand-in (pid %d)" w session;
            check (int_of_string_opt (String.trim (read_text (Filename.concat dir "waiter"))) = Some w) "the session's waiter file names %S, not the moth wait running (pid %d)" (read_text (Filename.concat dir "waiter")) w;
            track_app ();
            !app_say "bye"
        | _ -> ()
      in
      with_nothing_left "the window closed" (fun () -> app_ok ~stdin:true ~vars ~on_line [ "term"; bottle (); "moth-target.exe"; "3"; "stub agent up" ]))

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
    (13, "two-byte value", two_byte);
    (14, "eight-byte value", eight_byte);
    (15, "eight-byte decimal typed whole", eight_byte_decimal);
    (16, "a freed page", freed_page);
    (17, "tiny values", tiny_values);
    (18, "a float that did not move", still_float);
    (19, "a recalculated number", recalculated);
    (20, "keep, near, set by name", keep_near);
    (21, "keep by address", keep_address);
    (22, "pages", pages);
    (23, "over budget", over_budget);
    (24, "status after the game closes", closed_status);
    (25, "a value with no number", no_number);
    (26, "same first", same_first);
    (27, "snapshot speed", snapshot_speed);
    (28, "a snapshot over the limit", snapshot_over_limit);
    (29, "zero and one, fast", zero_and_one);
    (30, "put back", put_back);
    (31, "the feed", the_feed);
    (32, "launch", launched);
    (33, "the window", window);
    (34, "the list follows", list_follows);
    (35, "two hands on rows", two_hands);
    (36, "the game goes away", game_goes);
    (37, "an agent's hand", agents_hand);
    (38, "wait", waits);
    (39, "the agent in the window", agent_in_window);
    (40, "no agent", no_agent);
    (41, "the window goes with the agent up", window_goes_with_agent);
    (42, "an interrupted turn", interrupted_turn);
  ]

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
  Sys.set_signal Sys.sigpipe Sys.Signal_ignore;
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
