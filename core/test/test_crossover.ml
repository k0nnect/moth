open Moth_core

let check_err n w g = Alcotest.(check (result string string)) n (Error w) g
let rec rm p = if Sys.is_directory p then (Array.iter (fun f -> rm (Filename.concat p f)) (Sys.readdir p); Unix.rmdir p) else Sys.remove p

let mkdir_p p =
  let rec go p = if not (Sys.file_exists p) then (go (Filename.dirname p); Unix.mkdir p 0o755) in
  go p

let touch p = close_out (open_out p)

let with_tmp f =
  let d = Filename.concat (Filename.get_temp_dir_name ()) (Printf.sprintf "moth-cx-%d-%d" (Unix.getpid ()) (Random.bits ())) in
  mkdir_p d;
  Fun.protect ~finally:(fun () -> rm d) (fun () -> f d)

let with_env vars f =
  let old = List.map (fun (k, _) -> (k, Sys.getenv_opt k)) vars in
  List.iter (fun (k, v) -> Unix.putenv k v) vars;
  Fun.protect ~finally:(fun () -> List.iter (fun (k, o) -> Unix.putenv k (Option.value o ~default:"")) old) f

let app_tests () =
  with_tmp (fun tmp ->
      check_err "missing app" "crossover not found. set MOTH_CROSSOVER to the path of CrossOver.app."
        (with_env [ ("MOTH_CROSSOVER", "/nonexistent.app"); ("HOME", tmp) ] Crossover.find_app);
      let app = Filename.concat tmp "Applications/CrossOver.app" in
      mkdir_p app;
      Alcotest.(check (result string string)) "home app" (Ok app) (with_env [ ("MOTH_CROSSOVER", ""); ("HOME", tmp) ] Crossover.find_app);
      let other = Filename.concat tmp "Other.app" in
      mkdir_p other;
      Alcotest.(check (result string string)) "env wins" (Ok other) (with_env [ ("MOTH_CROSSOVER", other); ("HOME", tmp) ] Crossover.find_app))

let bottle_tests () =
  with_tmp (fun tmp ->
      let b = Filename.concat tmp "b" in
      List.iter (fun n -> mkdir_p (Filename.concat b n)) [ "Disco Elysium"; "uhmm"; "notabottle" ];
      touch (Filename.concat b "Disco Elysium/cxbottle.conf");
      touch (Filename.concat b "uhmm/cxbottle.conf");
      touch (Filename.concat b "stray-file");
      with_env [ ("MOTH_BOTTLES", b) ] (fun () ->
          Alcotest.(check (list string)) "bottles" [ "Disco Elysium"; "uhmm" ] (Crossover.list_bottles ());
          Alcotest.(check string) "dir" b (Crossover.bottles_dir ());
          Alcotest.(check (result unit string)) "known" (Ok ()) (Crossover.check_bottle "uhmm");
          check_err "unknown bottle" {|no bottle called "nope". bottles: Disco Elysium, uhmm|}
            (Result.map (fun () -> "") (Crossover.check_bottle "nope")));
      with_env [ ("MOTH_BOTTLES", ""); ("HOME", tmp) ] (fun () ->
          Alcotest.(check string) "default dir" (tmp ^ "/Library/Application Support/CrossOver/Bottles") (Crossover.bottles_dir ());
          Alcotest.(check (list string)) "none" [] (Crossover.list_bottles ())))

let argv () =
  let app = "/A/CrossOver.app" in
  Alcotest.(check (array string)) "argv"
    [| app ^ "/Contents/SharedSupport/CrossOver/bin/wine"; "--bottle"; "Disco Elysium"; "--workdir"; "/x/bin"; "/x/bin/moth-helper.exe"; "5000"; "tok" |]
    (Crossover.wine_argv ~app ~bottle:"Disco Elysium" ~exe:"/x/bin/moth-helper.exe" [ "5000"; "tok" ])

let () =
  Alcotest.run "crossover"
    [ ("crossover", [ Alcotest.test_case "app" `Quick app_tests; Alcotest.test_case "bottles" `Quick bottle_tests; Alcotest.test_case "argv" `Quick argv ]) ]
