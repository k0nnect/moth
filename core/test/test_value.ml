open Moth_core

let ok s v = Alcotest.(check bool) s true (Value.parse s = Ok v)
let bad s = Alcotest.(check bool) s true (Result.is_error (Value.parse s))

let cases () =
  ok "100" (Int 100L); ok " 1,250 " (Int 1250L); ok "-3" (Int (-3L)); ok "+5" (Int 5L); ok "1_000" (Int 1000L);
  ok "132.50" (Dec { text = "132.50"; value = 132.5; decimals = 2 });
  ok "-0.5" (Dec { text = "-0.5"; value = -0.5; decimals = 1 });
  ok "-9223372036854775808" (Int Int64.min_int);
  List.iter bad [ ""; "abc"; "85%"; "$5"; "1e5"; "12.3.4"; "."; "5."; "9223372036854775808"; "-"; ".5" ]

let () = Alcotest.run "value" [ ("parse", [ Alcotest.test_case "cases" `Quick cases ]) ]
