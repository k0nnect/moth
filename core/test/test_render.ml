open Moth_core

let check_str n w g = Alcotest.(check string) n w g

let outcome () =
  check_str "counts" "259,560 matches (241,178 whole number, 18,382 decimal)"
    (Render.outcome { main = 259560; wide = 0; by_tag = [ ("whole number", 241178); ("decimal", 18382) ]; notes = []; dead_end = false; ms = 0.; bytes = 0; unreadable = 0; groups = [] });
  check_str "dead end" "nothing left that fits. that step was not applied, so you can try a different number."
    (Render.outcome { main = 4; wide = 0; by_tag = []; notes = []; dead_end = true; ms = 0.; bytes = 0; unreadable = 0; groups = [] });
  check_str "fallback" "4 matches (4 whole number)\nno exact match for that number. kept the ones that went down instead."
    (Render.outcome { main = 4; wide = 0; by_tag = [ ("whole number", 4) ]; notes = [ Fell_back Down ]; dead_end = false; ms = 0.; bytes = 0; unreadable = 0; groups = [] });
  let plain ?(main = 0) ?(wide = 0) ?(dead_end = false) ?(unreadable = 0) by_tag notes : Session.outcome = { main; wide; by_tag; notes; dead_end; ms = 0.; bytes = 0; unreadable; groups = [] } in
  check_str "one match" "1 match (1 whole number)" (Render.outcome (plain ~main:1 [ ("whole number", 1) ] []));
  check_str "went up" "2 matches (2 decimal)\nno exact match for that number. kept the ones that went up instead."
    (Render.outcome (plain ~main:2 [ ("decimal", 2) ] [ Fell_back Up ]));
  check_str "stayed the same" "2 matches (2 decimal)\nno exact match for that number. kept the ones that stayed the same instead."
    (Render.outcome (plain ~main:2 [ ("decimal", 2) ] [ Fell_back Same ]));
  check_str "showing wide" "3 matches (3 whole number (1-byte))\nnothing matched as a plain number. showing other ways it could be stored."
    (Render.outcome (plain ~wide:3 [ ("whole number (1-byte)", 3) ] [ Showing_wide ]));
  check_str "untracked" "70 matches (50 whole number, 20 decimal)\nsome rarer forms are too common to track yet. they'll be rechecked on your next number."
    (Render.outcome (plain ~main:70 [ ("whole number", 50); ("decimal", 20) ] [ Untracked ]));
  check_str "lost" "12 matches (12 whole number)\n1,200 matches disappeared because the game freed that memory."
    (Render.outcome (plain ~main:12 [ ("whole number", 12) ] [ Lost 1200 ]));
  check_str "lost one" "12 matches (12 whole number)\n1 match disappeared because the game freed that memory."
    (Render.outcome (plain ~main:12 [ ("whole number", 12) ] [ Lost 1 ]));
  check_str "notes in order"
    "2 matches (2 whole number ×100)\n\
     no exact match for that number. kept the ones that went up instead.\n\
     nothing matched as a plain number. showing other ways it could be stored.\n\
     some rarer forms are too common to track yet. they'll be rechecked on your next number.\n\
     5 matches disappeared because the game freed that memory."
    (Render.outcome (plain ~wide:2 [ ("whole number ×100", 2) ] [ Lost 5; Untracked; Showing_wide; Fell_back Up ]));
  check_str "zero tags left out" "4 matches (4 whole number)" (Render.outcome (plain ~main:4 [ ("whole number", 4); ("decimal", 0) ] []));
  check_str "no matches" "0 matches" (Render.outcome (plain [] []));
  check_str "dead end alone" "nothing left that fits. that step was not applied, so you can try a different number."
    (Render.outcome (plain ~main:4 ~dead_end:true [ ("whole number", 4) ] [ Untracked; Lost 3 ]));
  check_str "unreadable"
    "3 matches (3 whole number)\ncouldn't read 4 kb of the game's memory, so matches there are missing."
    (Render.outcome (plain ~main:3 ~unreadable:4096 [ ("whole number", 3) ] []));
  check_str "unreadable before the notes"
    "2 matches (2 decimal)\n\
     couldn't read 12 mb of the game's memory, so matches there are missing.\n\
     no exact match for that number. kept the ones that went down instead.\n\
     5 matches disappeared because the game freed that memory."
    (Render.outcome (plain ~main:2 ~unreadable:(12 * 1048576) [ ("decimal", 2) ] [ Lost 5; Fell_back Down ]))

let sizes () =
  List.iter
    (fun (n, s) -> check_str s s (Render.size n))
    [
      (4096, "4 kb");
      (100, "1 kb");
      (1536, "2 kb");
      (1047552, "1023 kb");
      (1048064, "1 mb");
      (12 * 1048576, "12 mb");
      (1073741824 - 1048576, "1023 mb");
      (1073741824, "1.0 gb");
      (1288490189, "1.2 gb");
      (5 * 1073741824, "5.0 gb");
    ]

let counts () =
  let o ?(main = 0) ?(wide = 0) by_tag notes : Session.outcome = { main; wide; by_tag; notes; dead_end = false; ms = 0.; bytes = 0; unreadable = 0; groups = [] } in
  check_str "counts line" "4 matches (4 whole number)" (Render.counts (o ~main:4 [ ("whole number", 4) ] [ Fell_back Down; Untracked ]));
  check_str "wide when showing wide" "3 matches (3 whole number (1-byte))" (Render.counts (o ~wide:3 [ ("whole number (1-byte)", 3) ] [ Showing_wide ]));
  check_str "none" "0 matches" (Render.counts (o [] []))

let commas () =
  List.iter
    (fun (n, s) -> check_str s s (Render.commas n))
    [ (0, "0"); (7, "7"); (999, "999"); (1000, "1,000"); (18382, "18,382"); (1234567, "1,234,567"); (-1234, "-1,234"); (-999, "-999") ]

let rows () =
  check_str "rows" "#1  85    0x10008     whole number\n#2  85.5  0x7ffe0010  decimal"
    (Render.rows
       [
         { n = 1; addr = 0x10008L; interp_id = "i32"; tag = "whole number"; shown = "85"; away = None };
         { n = 2; addr = 0x7ffe0010L; interp_id = "f32"; tag = "decimal"; shown = "85.5"; away = None };
       ]);
  check_str "wide numbers" "#9   1.00  0xffff800000001000  whole number ×100\n#10  2     0x10                whole number"
    (Render.rows
       [
         { n = 9; addr = 0xffff800000001000L; interp_id = "i32x100"; tag = "whole number ×100"; shown = "1.00"; away = None };
         { n = 10; addr = 0x10L; interp_id = "i32"; tag = "whole number"; shown = "2"; away = None };
       ]);
  check_str "nothing to list" "nothing to list yet. narrow it down with your next number." (Render.rows [])

let () =
  Alcotest.run "render"
    [
      ( "render",
        [
          Alcotest.test_case "outcome" `Quick outcome;
          Alcotest.test_case "counts" `Quick counts;
          Alcotest.test_case "commas" `Quick commas;
          Alcotest.test_case "rows" `Quick rows;
          Alcotest.test_case "sizes" `Quick sizes;
        ] );
    ]
