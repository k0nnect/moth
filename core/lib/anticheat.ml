let table =
  [
    ("easy anti-cheat", [ "easyanticheat.exe"; "easyanticheat_eos.exe"; "easyanticheat_x64.dll"; "easyanticheat_x86.dll"; "eac_launcher.exe" ]);
    ("battleye", [ "beservice.exe"; "beservice_x64.exe"; "beclient.dll"; "beclient_x64.dll" ]);
    ("vanguard", [ "vgc.exe"; "vgtray.exe" ]);
    ("ea anticheat", [ "eaanticheat.gameservice.exe"; "eaanticheat.gameservicelauncher.exe" ]);
    ("punkbuster", [ "pnkbstra.exe"; "pnkbstrb.exe" ]);
    ("gameguard", [ "gamemon.des"; "gamemon64.des" ]);
    ("xigncode", [ "xxd-0.xem"; "x3.xem" ]);
    ("faceit", [ "faceit.exe"; "faceitclient.exe" ]);
    ("ace", [ "sguard64.exe"; "sguardsvc64.exe"; "ace-base.dll" ]);
  ]

let check ~processes ~modules =
  let names = List.map String.lowercase_ascii (processes @ modules) in
  List.find_map (fun (display, files) -> if List.exists (fun f -> List.mem f names) files then Some display else None) table
