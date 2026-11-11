let () = Alcotest.run "sanity" ["yojson", [Alcotest.test_case "parse" `Quick (fun () ->
  Alcotest.(check string) "a" "1" (Yojson.Safe.to_string (Yojson.Safe.Util.member "a" (Yojson.Safe.from_string {|{"a":1}|}))))]]
