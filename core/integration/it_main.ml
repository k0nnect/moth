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

let with_target ?(name = "moth-target.exe") ?(filler_mb = 0) ?(filler_small_mb = 0) ?(filler_tiny_mb = 0) f =
  let t = Target_ctl.start ~filler_small_mb ~filler_tiny_mb ~log:(target_log ()) ~exe:(exe name) ~filler_mb () in
  Fun.protect
    ~finally:(fun () -> Target_ctl.stop t)
    (fun () ->
      attach_said := to_string (member "message" (moth [ "attach"; "--bottle"; bottle (); name ]));
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
      check (List.exists (fun r -> r.addr = v.addr) rows) "no result at health_copy's address 0x%Lx. results: %s" v.addr (show_rows rows))

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
      check (String.starts_with ~prefix:"set #7 to 4 " said) "set #7 4 said %S" said;
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
  ]

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
