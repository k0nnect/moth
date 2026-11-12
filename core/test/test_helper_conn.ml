open Moth_core
open Helper_api

let check_bool n w g = Alcotest.(check bool) n w g
let log = Filename.concat (Filename.get_temp_dir_name ()) "moth-test-helper.log"
let ok = function Ok v -> v | Error m -> Alcotest.failf "unexpected error: %s" m
let is_code c = function Error { code; _ } -> code = c | Ok _ -> false

let connect port =
  let s = Unix.socket Unix.PF_INET Unix.SOCK_STREAM 0 in
  Unix.connect s (Unix.ADDR_INET (Unix.inet_addr_loopback, port));
  (s, Unix.in_channel_of_descr s, Unix.out_channel_of_descr s)

let send oc line = output_string oc (line ^ "\n"); flush oc
let hello token = Printf.sprintf {|{"hello":{"token":"%s","version":1,"pid":4242,"arch":"x86_64"}}|} token

let field name = function `Assoc l -> List.assoc name l | _ -> failwith "not an object"

(* the newest request line the fake helper read *)
let last_request = ref ""

(* how the fake helper answers: ps works, scan fails with not_attached, [`Lose] closes on ps, [`Wrong_id] answers with the wrong id *)
let serve ?(on_ps = `Answer) (ic, oc) =
  let rec loop () =
    match input_line ic with
    | exception End_of_file -> ()
    | line ->
        last_request := line;
        let j = Yojson.Safe.from_string line in
        let id = match field "id" j with `Int n -> n | _ -> -1 in
        let op = match field "op" j with `String s -> s | _ -> "" in
        (match op with
        | "ps" -> (
            match on_ps with
            | `Answer -> send oc (Printf.sprintf {|{"id":%d,"ok":true,"processes":[{"pid":208,"name":"disco.exe"}]}|} id)
            | `Wrong_id -> send oc (Printf.sprintf {|{"id":%d,"ok":true,"processes":[]}|} (id + 5))
            | `Lose -> raise Exit
            | `Error_wrong_id -> send oc (Printf.sprintf {|{"id":%d,"ok":false,"error":{"code":"not_attached","message":"x"}}|} (id + 5)))
        | "scan" -> send oc (Printf.sprintf {|{"id":%d,"ok":false,"error":{"code":"not_attached","message":"attach first"}}|} id)
        | "peek" when field "group" j = `String "h" ->
            send oc (Printf.sprintf {|{"id":%d,"ok":true,"items":[{"addr":"0x10","v":null},{"addr":"0x14","v":"7"}]}|} id)
        | "peek" when field "group" j = `String "f" ->
            send oc (Printf.sprintf {|{"id":%d,"ok":true,"items":[{"addr":"0x10","v":"nan"},{"addr":"0x14","v":85},{"addr":"0x18","v":"-inf"}]}|} id)
        | "peek" -> send oc (Printf.sprintf {|{"id":%d,"ok":true,"items":[{"addr":"0x10","v":"7"}]}|} id)
        | "write" -> send oc (Printf.sprintf {|{"id":%d,"ok":true,"prev":"inf","prev_bits":"0x7f800000"}|} id)
        | "status" -> send oc (Printf.sprintf {|{"id":%d,"ok":true,"attached":{"pid":208,"name":"disco.exe","bits":64},"groups":[]}|} id)
        | "bye" -> raise Exit
        | _ -> send oc (Printf.sprintf {|{"id":%d,"ok":true}|} id));
        loop ()
  in
  try loop () with Exit -> ()

let fake_helper ?on_ps ~tokens () ~port ~token ~log:_ =
  ignore
    (Thread.create
       (fun () ->
         List.iter
           (fun t ->
             let tok = match t with `Right -> token | `Wrong -> "wrong" in
             let s, ic, oc = connect port in
             send oc (hello tok);
             if t = `Right then serve ?on_ps (ic, oc);
             try Unix.close s with Unix.Unix_error _ -> ())
           tokens)
       ());
  Ok ()

let start ?on_ps ?(tokens = [ `Right ]) () = Helper_conn.start ~launch:(fake_helper ?on_ps ~tokens ()) ~label:"test" ~log ()

let scan_budget_and_peek_from () =
  let h = ok (start ()) in
  let sent () = Yojson.Safe.from_string !last_request in
  ignore (h.scan ~keep:false ~budget_mb:(Some 64) []);
  check_bool "budget sent" true (field "budget_mb" (sent ()) = `Int 64);
  ignore (h.scan ~keep:false ~budget_mb:None []);
  check_bool "no budget" true (match sent () with `Assoc l -> not (List.mem_assoc "budget_mb" l) | _ -> false);
  check_bool "peek_from" true (h.peek_from ~id:"i32" ~vtype:"i32" ~addr:0x6c551000L ~limit:64 = Ok [ { addr = 0x10L; v = Some (Vint 7L) } ]);
  check_bool "from, no offset" true
    (match sent () with
    | `Assoc l -> List.assoc_opt "from" l = Some (`String "0x6c551000") && not (List.mem_assoc "offset" l)
    | _ -> false);
  h.close ()

(* the core always asks for holes, and a position that can't be read comes back as None *)
let peek_asks_for_holes () =
  let h = ok (start ()) in
  let sent () = Yojson.Safe.from_string !last_request in
  let holes () = match sent () with `Assoc l -> List.assoc_opt "holes" l = Some (`Bool true) | _ -> false in
  check_bool "a hole" true (h.peek ~id:"h" ~vtype:"i32" ~offset:0 ~limit:5 = Ok [ { addr = 0x10L; v = None }; { addr = 0x14L; v = Some (Vint 7L) } ]);
  check_bool "peek asks for holes" true (holes ());
  ignore (h.peek_from ~id:"i32" ~vtype:"i32" ~addr:0x10L ~limit:5);
  check_bool "peek from asks for holes" true (holes ());
  h.close ()

let basics () =
  let h = ok (start ()) in
  check_bool "ps" true (h.ps () = Ok [ (208, "disco.exe") ]);
  check_bool "peek" true (h.peek ~id:"i32" ~vtype:"i32" ~offset:0 ~limit:5 = Ok [ { addr = 0x10L; v = Some (Vint 7L) } ]);
  check_bool "error mapped" true (match h.scan ~keep:false ~budget_mb:None [] with Error { code = "not_attached"; _ } -> true | _ -> false);
  check_bool "status" true (h.status () = Ok (Some { pid = 208; name = "disco.exe"; bits = 64 }));
  check_bool "detach" true (h.detach () = Ok ());
  h.close ()

let typed_values () =
  let h = ok (start ()) in
  (match h.peek ~id:"f" ~vtype:"f32" ~offset:0 ~limit:5 with
  | Ok [ { addr = 0x10L; v = Some (Vfloat a) }; { addr = 0x14L; v = Some (Vfloat b) }; { addr = 0x18L; v = Some (Vfloat c) } ] ->
      check_bool "nan" true (Float.is_nan a);
      check_bool "number" true (b = 85.);
      check_bool "-inf" true (c = Float.neg_infinity)
  | _ -> Alcotest.fail "peek of a float group");
  check_bool "a whole number as its type" true (h.peek ~id:"u" ~vtype:"u64" ~offset:0 ~limit:5 = Ok [ { addr = 0x10L; v = Some (Vint 7L) } ]);
  check_bool "write a value" true (h.write ~addr:0x10L ~vtype:"f32" (Put_value (Vfloat 2.5)) = Ok { prev = Vfloat Float.infinity; prev_bits = 0x7f800000L });
  check_bool "sends v" true (Yojson.Safe.from_string !last_request = Yojson.Safe.from_string {|{"id":3,"op":"write","addr":"0x10","type":"f32","v":2.5}|});
  ignore (h.write ~addr:0x10L ~vtype:"f32" (Put_bits 0x7fc00001L));
  check_bool "sends bits" true (Yojson.Safe.from_string !last_request = Yojson.Safe.from_string {|{"id":4,"op":"write","addr":"0x10","type":"f32","bits":"0x7fc00001"}|});
  h.close ()

let wrong_token () =
  let h = ok (start ~tokens:[ `Wrong; `Right ] ()) in
  check_bool "accepted second" true (Result.is_ok (h.ps ()));
  h.close ()

let never_connects () =
  Alcotest.(check (result unit string)) "timeout" (Error ("the helper didn't start in test (waited 1s). see " ^ log))
    (Result.map ignore (Helper_conn.start ~launch:(fun ~port:_ ~token:_ ~log:_ -> Ok ()) ~label:"test" ~log ~timeout:1.0 ()))

let launch_fails () =
  Alcotest.(check (result unit string)) "launch error" (Error "boom")
    (Result.map ignore (Helper_conn.start ~launch:(fun ~port:_ ~token:_ ~log:_ -> Error "boom") ~label:"test" ~log ()))

let lost () =
  let h = ok (start ~on_ps:`Lose ()) in
  check_bool "lost" true (is_code "helper_lost" (h.ps ()));
  check_bool "stays lost" true (is_code "helper_lost" (h.ps ()));
  h.close ()

let wrong_id () =
  let h = ok (start ~on_ps:`Wrong_id ()) in
  check_bool "mismatch is lost" true (is_code "helper_lost" (h.ps ()));
  check_bool "closed after" true (is_code "helper_lost" (h.ps ()))

let error_wrong_id () =
  let h = ok (start ~on_ps:`Error_wrong_id ()) in
  check_bool "error with wrong id is lost" true (is_code "helper_lost" (h.ps ()));
  check_bool "closed after" true (is_code "helper_lost" (h.ps ()))

let silent_connection () =
  let silent = ref None in
  let launch ~port ~token ~log =
    let s, _, _ = connect port in
    silent := Some s;
    fake_helper ~tokens:[ `Right ] () ~port ~token ~log
  in
  let h = ok (Helper_conn.start ~launch ~label:"test" ~log ~hello_timeout:0.3 ()) in
  check_bool "ps after silent peer" true (Result.is_ok (h.ps ()));
  h.close ();
  Option.iter Unix.close !silent

let oversized_hello () =
  let launch ~port ~token ~log =
    let s, _, oc = connect port in
    output_string oc (String.make 10000 'x');
    flush oc;
    ignore s;
    fake_helper ~tokens:[ `Right ] () ~port ~token ~log
  in
  let h = ok (Helper_conn.start ~launch ~label:"test" ~log ()) in
  check_bool "ps" true (Result.is_ok (h.ps ()));
  h.close ()

let relative_helper () =
  let old = Sys.getenv_opt "MOTH_HELPER" in
  Unix.putenv "MOTH_HELPER" "test_helper_conn.exe";
  let r = Helper_conn.helper_exe () in
  Unix.putenv "MOTH_HELPER" (Option.value old ~default:"");
  Alcotest.(check (result string string)) "absolute" (Ok (Filename.concat (Sys.getcwd ()) "test_helper_conn.exe")) r

let () =
  Alcotest.run "helper_conn"
    [
      ( "conn",
        [
          Alcotest.test_case "requests" `Quick basics;
          Alcotest.test_case "scan budget, peek from" `Quick scan_budget_and_peek_from;
          Alcotest.test_case "peek asks for holes" `Quick peek_asks_for_holes;
          Alcotest.test_case "values by type, writes by value or bytes" `Quick typed_values;
          Alcotest.test_case "wrong token" `Quick wrong_token;
          Alcotest.test_case "never connects" `Quick never_connects;
          Alcotest.test_case "launch fails" `Quick launch_fails;
          Alcotest.test_case "connection lost" `Quick lost;
          Alcotest.test_case "wrong id" `Quick wrong_id;
          Alcotest.test_case "error with wrong id" `Quick error_wrong_id;
          Alcotest.test_case "silent connection" `Quick silent_connection;
          Alcotest.test_case "oversized hello" `Quick oversized_hello;
          Alcotest.test_case "relative MOTH_HELPER" `Quick relative_helper;
        ] );
    ]
