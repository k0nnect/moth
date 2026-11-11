open Moth_core
open Helper_api

let check_str n w g = Alcotest.(check string) n w g
let check_bool n w g = Alcotest.(check bool) n w g
let js = Yojson.Safe.to_string
let raises_failure f = match f () with exception Failure _ -> true | _ -> false

let requests () =
  check_str "scan request" {|{"id":3,"op":"scan","keep":false,"groups":[{"id":"i32","type":"i32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"100"}}]}|}
    (Wire.request ~id:3 ~op:"scan" [ "keep", `Bool false; "groups", `List [ Wire.group_spec_to_json { id = "i32"; vtype = "i32"; align = 4; exclude_align = 0; pred = Eq 100L } ] ]);
  check_str "no fields" {|{"id":1,"op":"ps"}|} (Wire.request ~id:1 ~op:"ps" []);
  check_str "negative eq" {|{"op":"eq","v":"-3"}|} (js (Wire.pred_to_json (Eq (-3L))));
  check_str "range" {|{"op":"range","lo":99.0,"hi":101.0}|} (js (Wire.pred_to_json (Range (99., 101.))));
  check_str "lt_prev" {|{"op":"lt_prev"}|} (js (Wire.pred_to_json Lt_prev));
  check_str "gt_prev" {|{"op":"gt_prev"}|} (js (Wire.pred_to_json Gt_prev));
  check_str "eq_prev" {|{"op":"eq_prev"}|} (js (Wire.pred_to_json Eq_prev));
  check_str "ne_prev" {|{"op":"ne_prev"}|} (js (Wire.pred_to_json Ne_prev));
  check_str "unaligned spec" {|{"id":"f32u","type":"f32","align":1,"exclude_align":4,"pred":{"op":"range","lo":0.5,"hi":1.5}}|}
    (js (Wire.group_spec_to_json { id = "f32u"; vtype = "f32"; align = 1; exclude_align = 4; pred = Range (0.5, 1.5) }));
  check_str "int value" {|"-3"|} (js (Wire.value_to_json (Vint (-3L))));
  check_str "float value" "85.5" (js (Wire.value_to_json (Vfloat 85.5)));
  check_str "addr" {|"0x1f3a0010"|} (js (Wire.addr_to_json 0x1f3a0010L));
  check_str "addr above 2^63" {|"0xffff800000001000"|} (js (Wire.addr_to_json 0xffff800000001000L));
  check_str "write request" {|{"id":7,"op":"write","addr":"0x10008","type":"i32","v":"999"}|}
    (Wire.request ~id:7 ~op:"write" [ "addr", Wire.addr_to_json 0x10008L; "type", `String "i32"; "v", Wire.value_to_json (Vint 999L) ])

let replies () =
  check_bool "error reply" true (Wire.parse_reply {|{"id":4,"ok":false,"error":{"code":"target_gone","message":"x"}}|} = Error { code = "target_gone"; message = "x" });
  check_bool "error reply without id" true
    (Wire.parse_reply {|{"ok":false,"error":{"code":"bad_request","message":"request must be a JSON object"}}|}
    = Error { code = "bad_request"; message = "request must be a JSON object" });
  (match Wire.parse_reply {|{"applied":true,"id":5,"ok":true}|} with
  | Ok (id, body) ->
      Alcotest.(check int) "id" 5 id;
      check_bool "body" true (Yojson.Safe.Util.member "applied" body = `Bool true)
  | Error e -> Alcotest.failf "ok reply: %s" e.message);
  let bad s = match Wire.parse_reply s with Error { code = "bad_reply"; _ } -> true | _ -> false in
  check_bool "not json" true (bad "nope");
  check_bool "not an object" true (bad "[1]");
  check_bool "no ok" true (bad {|{"id":1}|});
  check_bool "ok without id" true (bad {|{"ok":true}|});
  check_bool "error without code" true (bad {|{"id":1,"ok":false}|})

let decoders () =
  let j = Yojson.Safe.from_string in
  check_bool "items" true (Wire.items ~vtype:"i32" (`List [ `Assoc [ "addr", `String "0x10008"; "v", `String "100" ] ]) = [ { addr = 0x10008L; v = Vint 100L } ]);
  check_bool "high address" true
    (Wire.items ~vtype:"f32" (j {|[{"addr":"0xffffffffffff0010","v":85.5}]|}) = [ { addr = 0xffffffffffff0010L; v = Vfloat 85.5 } ]);
  check_bool "groups" true
    (Wire.group_infos (j {|[{"count":2,"has_values":true,"id":"i32","tracked":true},{"count":9000000,"has_values":false,"id":"u8","tracked":false}]|})
    = [ { id = "i32"; count = 2; tracked = true; has_values = true }; { id = "u8"; count = 9000000; tracked = false; has_values = false } ]);
  check_bool "stats" true
    (Wire.stats (j {|{"bytes":4096,"lost":1,"ms":2.5,"regions":3,"unreadable_bytes":16}|})
    = { regions = 3; bytes = 4096; unreadable_bytes = 16; lost = 1; ms = 2.5 });
  check_bool "whole ms" true ((Wire.stats (j {|{"bytes":0,"lost":0,"ms":2,"regions":0,"unreadable_bytes":0}|})).ms = 2.);
  check_bool "u64" true (Wire.value_of_json ~vtype:"u64" (`String "18446744073709551615") = Vint (-1L));
  check_bool "i8" true (Wire.value_of_json ~vtype:"i8" (`String "-128") = Vint (-128L));
  check_bool "f64" true (Wire.value_of_json ~vtype:"f64" (`Float 0.25) = Vfloat 0.25);
  check_bool "float printed whole" true (Wire.value_of_json ~vtype:"f32" (`Int 100) = Vfloat 100.);
  check_bool "int needs a string" true (raises_failure (fun () -> Wire.value_of_json ~vtype:"i32" (`Int 5)));
  check_bool "not decimal" true (raises_failure (fun () -> Wire.value_of_json ~vtype:"i32" (`String "0x10")));
  check_bool "float needs a number" true (raises_failure (fun () -> Wire.value_of_json ~vtype:"f32" (`String "1.5")));
  check_bool "unknown type" true (raises_failure (fun () -> Wire.value_of_json ~vtype:"i128" (`String "1")));
  check_bool "bad address" true (raises_failure (fun () -> Wire.items ~vtype:"i32" (j {|[{"addr":"10008","v":"1"}]|})));
  check_bool "address too long" true (raises_failure (fun () -> Wire.items ~vtype:"i32" (j {|[{"addr":"0x10000000000000000","v":"1"}]|})));
  check_bool "missing field" true (raises_failure (fun () -> Wire.group_infos (j {|[{"id":"i32","count":1}]|})))

let () =
  Alcotest.run "wire"
    [ ("wire", [ Alcotest.test_case "requests" `Quick requests; Alcotest.test_case "replies" `Quick replies; Alcotest.test_case "decoders" `Quick decoders ]) ]
