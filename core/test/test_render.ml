open Moth_core

let check_str n w g = Alcotest.(check string) n w g
let check_bool n w g = Alcotest.(check bool) n w g
let last_line s = List.nth (String.split_on_char '\n' s) (List.length (String.split_on_char '\n' s) - 1)

let outcome () =
  check_str "counts" "259,560 matches (241,178 whole number, 18,382 decimal)"
    (Render.outcome { main = 259560; wide = 0; by_tag = [ ("whole number", 241178); ("decimal", 18382) ]; notes = []; dead_end = false; by_relation = false; ms = 0.; bytes = 0; unreadable = 0; groups = [] });
  check_str "dead end" "nothing left that fits. that step was not applied, so you can try a different number, or start over by how it moves: moth snapshot"
    (Render.outcome { main = 4; wide = 0; by_tag = []; notes = []; dead_end = true; by_relation = false; ms = 0.; bytes = 0; unreadable = 0; groups = [] });
  (* R12: what a dead end says follows the step and whether the hunt can be compared by change *)
  let comparable : Helper_api.group_info = { id = "i32"; count = 4; tracked = true; has_values = true } in
  let dead ~by_relation groups : Session.outcome =
    { main = 4; wide = 0; by_tag = [ ("whole number", 4) ]; notes = []; dead_end = true; by_relation; ms = 0.; bytes = 0; unreadable = 0; groups }
  in
  check_str "dead end after a relation step"
    "nothing fits that, so that step was not applied. say how it moved again: moth next down, up, same or changed. or type the number it shows: moth next <number>"
    (Render.outcome (dead ~by_relation:true [ (comparable, Interp.Main) ]));
  check_str "dead end after a number, comparable by change"
    "nothing left that fits. that step was not applied, so you can try a different number, or say how it moved: moth next down, up, same or changed"
    (Render.outcome (dead ~by_relation:false [ (comparable, Interp.Main) ]));
  List.iter
    (fun (why, (g : Helper_api.group_info)) ->
      check_str ("dead end after a number: " ^ why)
        "nothing left that fits. that step was not applied, so you can try a different number, or start over by how it moves: moth snapshot"
        (Render.outcome (dead ~by_relation:false [ (g, Interp.Main) ])))
    [
      ("no values", { comparable with has_values = false });
      ("untracked", { comparable with tracked = false });
      ("no matches", { comparable with count = 0 });
    ];
  check_str "fallback" "4 matches (4 whole number)\nno exact match for that number. kept the ones that went down instead."
    (Render.outcome { main = 4; wide = 0; by_tag = [ ("whole number", 4) ]; notes = [ Fell_back Down ]; dead_end = false; by_relation = false; ms = 0.; bytes = 0; unreadable = 0; groups = [] });
  let plain ?(main = 0) ?(wide = 0) ?(dead_end = false) ?(unreadable = 0) by_tag notes : Session.outcome = { main; wide; by_tag; notes; dead_end; by_relation = false; ms = 0.; bytes = 0; unreadable; groups = [] } in
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
  check_str "dead end alone" "nothing left that fits. that step was not applied, so you can try a different number, or start over by how it moves: moth snapshot"
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
  let o ?(main = 0) ?(wide = 0) by_tag notes : Session.outcome = { main; wide; by_tag; notes; dead_end = false; by_relation = false; ms = 0.; bytes = 0; unreadable = 0; groups = [] } in
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

let rows_near () =
  check_str "the distance after the address" "#1  532  0x6c551078  +16  whole number\n#2  2    0x6c551060  -8   whole number\n#3  15   0x6c551068  0    whole number"
    (Render.rows
       [
         { n = 1; addr = 0x6c551078L; interp_id = "i32"; tag = "whole number"; shown = "532"; away = Some 16 };
         { n = 2; addr = 0x6c551060L; interp_id = "i32"; tag = "whole number"; shown = "2"; away = Some (-8) };
         { n = 3; addr = 0x6c551068L; interp_id = "i32"; tag = "whole number"; shown = "15"; away = Some 0 };
       ])

let listing () =
  let row n : Session.row = { n; addr = Int64.of_int (0x1000 + (4 * n)); interp_id = "i32"; tag = "whole number"; shown = "3"; away = None } in
  let page rows total : Session.listing = { rows; total; from = (List.hd rows).n } in
  check_str "the total with separators" "showing 1-3 of 10,245. more: moth results --from 4" (last_line (Render.listing ~all:false (page [ row 1; row 2; row 3 ] 10245)));
  check_str "and the rows" "showing 1,001-1,002 of 10,245. more: moth results --all --from 1003 --near xp"
    (last_line (Render.listing ~near:"xp" ~all:true (page [ row 1001; row 1002 ] 10245)));
  check_str "small numbers as before" "showing 21-22 of 30. more: moth results --from 23" (last_line (Render.listing ~all:false (page [ row 21; row 22 ] 30)));
  check_bool "the last page has no such line" false (String.contains (Render.listing ~all:false (page [ row 1; row 2 ] 2)) 's')

let kept () =
  check_str "kept" "xp     95    0x6c551068  whole number\ncents  5.32  0x6c551078  whole number ×100\nhp     ?     0x10        decimal"
    (Render.kept
       [
         { name = "xp"; kaddr = 0x6c551068L; ktag = "whole number"; kshown = "95"; set_to = None; put_back = false };
         { name = "cents"; kaddr = 0x6c551078L; ktag = "whole number ×100"; kshown = "5.32"; set_to = None; put_back = false };
         { name = "hp"; kaddr = 0x10L; ktag = "decimal"; kshown = "?"; set_to = None; put_back = false };
       ]);
  check_str "none" "nothing kept yet. keep a result with: moth keep <#n> [name]" (Render.kept [])

(* R14: a value that reads as it did before its newest write says so after its tag; other lines are
   as before *)
let kept_note () =
  let k ?set_to ?(put_back = false) kshown : Session.kept = { name = "xp"; kaddr = 0x10L; ktag = "whole number"; kshown; set_to; put_back } in
  check_str "back" "you set it to 20; it is back to what it was before." (Render.kept_note (k ~set_to:"20" ~put_back:true "13"));
  check_str "set, holding" "" (Render.kept_note (k ~set_to:"30" "30"));
  check_str "never set" "" (Render.kept_note (k "95"))

let kept_put_back () =
  let k ?set_to ?(put_back = false) name kshown : Session.kept = { name; kaddr = 0x10L; ktag = "whole number"; kshown; set_to; put_back } in
  check_str "put back"
    "xp    95  0x10  whole number\nconc  13  0x10  whole number  you set it to 20; it is back to what it was before.\nhp    30  0x10  whole number"
    (Render.kept [ k "xp" "95"; k ~set_to:"20" ~put_back:true "conc" "13"; k ~set_to:"30" "hp" "30" ])

(* a snapshot's line takes the counts line's place; the unreadable line and the notes follow *)
let snapshot_taken () =
  let o ?(unreadable = 0) notes : Session.outcome =
    { main = 5000; wide = 4000; by_tag = [ ("whole number", 1000); ("decimal", 1000) ]; notes; dead_end = false; by_relation = false; ms = 0.; bytes = 12 * 1048576; unreadable; groups = [] }
  in
  let line = "snapshot of 12 mb taken. change the value in the game, then tell moth how it moved: moth next down, up, same or changed." in
  check_str "one line" line (Render.outcome (o [ Snapshot_taken ]));
  check_str "then the unreadable line" (line ^ "\ncouldn't read 4 kb of the game's memory, so matches there are missing.")
    (Render.outcome (o ~unreadable:4096 [ Snapshot_taken ]));
  check_str "then the notes"
    (line ^ "\nsome rarer forms are too common to track yet. they'll be rechecked on your next number.")
    (Render.outcome (o [ Snapshot_taken; Untracked ]))

let () =
  Alcotest.run "render"
    [
      ( "render",
        [
          Alcotest.test_case "outcome" `Quick outcome;
          Alcotest.test_case "counts" `Quick counts;
          Alcotest.test_case "commas" `Quick commas;
          Alcotest.test_case "rows" `Quick rows;
          Alcotest.test_case "rows near a value" `Quick rows_near;
          Alcotest.test_case "a page of results" `Quick listing;
          Alcotest.test_case "kept" `Quick kept;
          Alcotest.test_case "kept, put back" `Quick kept_put_back;
          Alcotest.test_case "kept note" `Quick kept_note;
          Alcotest.test_case "snapshot taken" `Quick snapshot_taken;
          Alcotest.test_case "sizes" `Quick sizes;
        ] );
    ]
