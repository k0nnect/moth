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

(* how the fake helper answers: ps works, scan fails with not_attached, [`Lose] closes on ps, [`Wrong_id] answers with the wrong id *)
let serve ?(on_ps = `Answer) (ic, oc) =
  let rec loop () =
    match input_line ic with
    | exception End_of_file -> ()
    | line ->
        let j = Yojson.Safe.from_string line in
        let id = match field "id" j with `Int n -> n | _ -> -1 in
        let op = match field "op" j with `String s -> s | _ -> "" in
        (match op with
        | "ps" -> (
            match on_ps with
            | `Answer -> send oc (Printf.sprintf {|{"id":%d,"ok":true,"processes":[{"pid":208,"name":"disco.exe"}]}|} id)
            | `Wrong_id -> send oc (Printf.sprintf {|{"id":%d,"ok":true,"processes":[]}|} (id + 5))
            | `Lose -> raise Exit)
        | "scan" -> send oc (Printf.sprintf {|{"id":%d,"ok":false,"error":{"code":"not_attached","message":"attach first"}}|} id)
        | "peek" -> send oc (Printf.sprintf {|{"id":%d,"ok":true,"items":[{"addr":"0x10","v":"7"}]}|} id ^ "")
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

let basics () =
  let h = ok (start ()) in
  check_bool "ps" true (h.ps () = Ok [ (208, "disco.exe") ]);
  check_bool "peek" true (h.peek ~id:"i32" ~offset:0 ~limit:5 = Ok [ { addr = 0x10L; v = Vint 7L } ]);
  check_bool "error mapped" true (match h.scan ~keep:false [] with Error { code = "not_attached"; _ } -> true | _ -> false);
  check_bool "detach" true (h.detach () = Ok ());
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

let () =
  Alcotest.run "helper_conn"
    [
      ( "conn",
        [
          Alcotest.test_case "requests" `Quick basics;
          Alcotest.test_case "wrong token" `Quick wrong_token;
          Alcotest.test_case "never connects" `Quick never_connects;
          Alcotest.test_case "launch fails" `Quick launch_fails;
          Alcotest.test_case "connection lost" `Quick lost;
          Alcotest.test_case "wrong id" `Quick wrong_id;
        ] );
    ]
