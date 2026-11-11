open Moth_core

let check_opt n w g = Alcotest.(check (option string)) n w g

let cases () =
  check_opt "eac" (Some "easy anti-cheat") (Anticheat.check ~processes:[ "game.exe"; "EasyAntiCheat_EOS.exe" ] ~modules:[]);
  check_opt "battleye module" (Some "battleye") (Anticheat.check ~processes:[] ~modules:[ "BEClient_x64.dll" ]);
  check_opt "vanguard" (Some "vanguard") (Anticheat.check ~processes:[ "VGC.exe" ] ~modules:[]);
  check_opt "ace" (Some "ace") (Anticheat.check ~processes:[] ~modules:[ "ACE-Base.dll" ]);
  check_opt "clean" None (Anticheat.check ~processes:[ "disco.exe"; "explorer.exe" ] ~modules:[ "UnityPlayer.dll"; "GameAssembly.dll" ])

let () = Alcotest.run "anticheat" [ ("check", [ Alcotest.test_case "names" `Quick cases ]) ]
