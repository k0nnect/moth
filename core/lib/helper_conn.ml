open Helper_api

type launcher = port:int -> token:string -> log:string -> (unit, string) result

let lost message = Error { code = "helper_lost"; message }
let now = Unix.gettimeofday

(* ---- lines over a socket, with deadlines ---- *)

type conn = { fd : Unix.file_descr; buf : Buffer.t; mutable closed : bool; mutable next_id : int }

let new_conn fd = { fd; buf = Buffer.create 4096; closed = false; next_id = 1 }

let close_conn c =
  if not c.closed then begin
    c.closed <- true;
    try Unix.close c.fd with Unix.Unix_error _ -> ()
  end

let take_line c =
  let s = Buffer.contents c.buf in
  match String.index_opt s '\n' with
  | None -> None
  | Some i ->
      Buffer.clear c.buf;
      Buffer.add_string c.buf (String.sub s (i + 1) (String.length s - i - 1));
      Some (String.sub s 0 i)

let rec readable fd ~deadline =
  let left = deadline -. now () in
  if left <= 0. then false
  else
    match Unix.select [ fd ] [] [] left with
    | [], _, _ -> readable fd ~deadline
    | _ -> true
    | exception Unix.Unix_error (Unix.EINTR, _, _) -> readable fd ~deadline

let chunk = Bytes.create 4096

let rec read_line ?(max = max_int) c ~deadline =
  match take_line c with
  | Some l -> `Line l
  | None ->
      if Buffer.length c.buf > max then `Eof
      else if not (readable c.fd ~deadline) then `Timeout
      else (
        match Unix.read c.fd chunk 0 (Bytes.length chunk) with
        | 0 -> `Eof
        | n ->
            Buffer.add_subbytes c.buf chunk 0 n;
            read_line ~max c ~deadline
        | exception Unix.Unix_error (Unix.EINTR, _, _) -> read_line ~max c ~deadline
        | exception Unix.Unix_error _ -> `Eof)

let write_all c s =
  let b = Bytes.of_string s in
  let rec go off =
    if off < Bytes.length b then
      match Unix.write c.fd b off (Bytes.length b - off) with
      | n -> go (off + n)
      | exception Unix.Unix_error (Unix.EINTR, _, _) -> go off
  in
  try go 0; true with Unix.Unix_error _ -> false

(* ---- requests ---- *)

let call c ~timeout ~op fields =
  if c.closed then lost "the helper connection is closed"
  else begin
    let id = c.next_id in
    c.next_id <- id + 1;
    let fail msg =
      close_conn c;
      lost msg
    in
    if not (write_all c (Wire.request ~id ~op fields ^ "\n")) then fail "could not send to the helper"
    else
      match read_line c ~deadline:(now () +. timeout) with
      | `Eof -> fail "the helper closed the connection"
      | `Timeout -> fail (Printf.sprintf "the helper did not answer %s within %.0fs" op timeout)
      | `Line l -> (
          let mismatch =
            match Yojson.Safe.from_string l with
            | `Assoc f -> ( match List.assoc_opt "id" f with Some (`Int rid) when rid <> id -> Some rid | _ -> None)
            | _ | (exception Yojson.Json_error _) -> None
          in
          match (mismatch, Wire.parse_reply l) with
          | Some rid, _ -> fail (Printf.sprintf "the helper answered request %d to request %d" rid id)
          | None, Error { code = "bad_reply"; message } -> fail ("the helper sent something that is not a reply: " ^ message)
          | None, Error e -> Error e
          | None, Ok (_, reply) -> Ok reply)
  end

let decode f reply = try Ok (f reply) with Failure m -> Error { code = "bad_reply"; message = m }
let ( let* ) = Result.bind

let make c =
  let short = 30. and long = 300. in
  let req ?(timeout = short) op fields f =
    let* reply = call c ~timeout ~op fields in
    decode f reply
  in
  let field name = function `Assoc l -> ( match List.assoc_opt name l with Some v -> v | None -> failwith ("missing " ^ name)) | _ -> failwith "reply is not an object" in
  let int = function `Int n -> n | _ -> failwith "expected an integer" in
  let str = function `String s -> s | _ -> failwith "expected a string" in
  let list = function `List l -> l | _ -> failwith "expected a list" in
  let attached r = { pid = int (field "pid" r); name = str (field "name" r); bits = int (field "bits" r) } in
  {
    ps = (fun () -> req "ps" [] (fun r -> List.map (fun p -> (int (field "pid" p), str (field "name" p))) (list (field "processes" r))));
    modules = (fun pid -> req "modules" [ ("pid", `Int pid) ] (fun r -> List.map str (list (field "modules" r))));
    attach = (fun pid -> req "attach" [ ("pid", `Int pid) ] attached);
    status = (fun () -> req "status" [] (fun r -> match field "attached" r with `Null -> None | a -> Some (attached a)));
    scan =
      (fun ~keep ~budget_mb specs ->
        req ~timeout:long "scan"
          ([ ("groups", `List (List.map Wire.group_spec_to_json specs)); ("keep", `Bool keep) ]
          @ match budget_mb with Some n -> [ ("budget_mb", `Int n) ] | None -> [])
          (fun r -> (Wire.group_infos (field "groups" r), Wire.stats (field "stats" r))));
    narrow =
      (fun ~atomic items ->
        let groups = List.map (fun (id, pred) -> `Assoc [ ("id", `String id); ("pred", Wire.pred_to_json pred) ]) items in
        req ~timeout:long "narrow"
          [ ("groups", `List groups); ("atomic", `Bool atomic) ]
          (fun r ->
            let applied = match field "applied" r with `Bool b -> b | _ -> failwith "\"applied\" must be true or false" in
            (applied, Wire.group_infos (field "groups" r), Wire.stats (field "stats" r))));
    drop = (fun ids -> req "drop" [ ("ids", `List (List.map (fun i -> `String i) ids)) ] ignore);
    (* both forms ask for holes: a position that can't be read keeps its place in the page *)
    peek =
      (fun ~id ~vtype ~offset ~limit ->
        req "peek"
          [ ("group", `String id); ("offset", `Int offset); ("limit", `Int limit); ("holes", `Bool true) ]
          (fun r -> Wire.items ~vtype (field "items" r)));
    peek_from =
      (fun ~id ~vtype ~addr ~limit ->
        req "peek"
          [ ("group", `String id); ("from", Wire.addr_to_json addr); ("limit", `Int limit); ("holes", `Bool true) ]
          (fun r -> Wire.items ~vtype (field "items" r)));
    read =
      (fun ~addr ~vtype ->
        req "read" [ ("addr", Wire.addr_to_json addr); ("type", `String vtype) ] (fun r -> Wire.value_of_json ~vtype (field "v" r)));
    write =
      (fun ~addr ~vtype put ->
        let what = match put with Put_value v -> ("v", Wire.value_to_json v) | Put_bits b -> ("bits", Wire.bits_to_json ~vtype b) in
        req "write"
          [ ("addr", Wire.addr_to_json addr); ("type", `String vtype); what ]
          (fun r -> { prev = Wire.value_of_json ~vtype (field "prev" r); prev_bits = Wire.bits_of_json ~vtype (field "prev_bits" r) }));
    detach = (fun () -> req "detach" [] ignore);
    close =
      (fun () ->
        if not c.closed then begin
          ignore (write_all c (Wire.request ~id:c.next_id ~op:"bye" [] ^ "\n"));
          close_conn c
        end);
  }

(* ---- starting ---- *)

let token () =
  let ic = open_in_bin "/dev/urandom" in
  let raw = Fun.protect ~finally:(fun () -> close_in ic) (fun () -> really_input_string ic 16) in
  String.concat "" (List.init 16 (fun i -> Printf.sprintf "%02x" (Char.code raw.[i])))

let hello_ok ~token line =
  match Yojson.Safe.from_string line with
  | `Assoc [ ("hello", `Assoc h) ] -> List.assoc_opt "token" h = Some (`String token)
  | _ -> false
  | exception Yojson.Json_error _ -> false

let rec accept_hello listener ~token ~deadline ~hello_timeout =
  if not (readable listener ~deadline) then None
  else
    match Unix.accept ~cloexec:true listener with
    | exception Unix.Unix_error _ -> accept_hello listener ~token ~deadline ~hello_timeout
    | fd, _ -> (
        let c = new_conn fd in
        match read_line ~max:4096 c ~deadline:(Float.min deadline (now () +. hello_timeout)) with
        | `Line l when hello_ok ~token l -> Some c
        | _ ->
            close_conn c;
            accept_hello listener ~token ~deadline ~hello_timeout)

let start ~launch ~label ~log ?(timeout = 60.) ?(hello_timeout = 5.) () =
  Sys.set_signal Sys.sigpipe Sys.Signal_ignore;
  let listener = Unix.socket ~cloexec:true Unix.PF_INET Unix.SOCK_STREAM 0 in
  Fun.protect
    ~finally:(fun () -> Unix.close listener)
    (fun () ->
      Unix.bind listener (Unix.ADDR_INET (Unix.inet_addr_loopback, 0));
      Unix.listen listener 4;
      let port = match Unix.getsockname listener with Unix.ADDR_INET (_, p) -> p | _ -> 0 in
      let token = token () in
      let deadline = now () +. timeout in
      match launch ~port ~token ~log with
      | Error m -> Error m
      | Ok () -> (
          match accept_hello listener ~token ~deadline ~hello_timeout with
          | Some c -> Ok (make c)
          | None -> Error (Printf.sprintf "the helper didn't start in %s (waited %.0fs). see %s" label timeout log)))

let wine_launcher ~bottle ~helper_exe ~port ~token ~log =
  let* app = Crossover.find_app () in
  let* () = Crossover.check_bottle bottle in
  let argv = Crossover.wine_argv ~app ~bottle ~exe:helper_exe [ string_of_int port; token ] in
  match
    let out = Unix.openfile log [ Unix.O_WRONLY; Unix.O_CREAT; Unix.O_APPEND; Unix.O_CLOEXEC ] 0o644 in
    let null = Unix.openfile "/dev/null" [ Unix.O_RDONLY; Unix.O_CLOEXEC ] 0 in
    Fun.protect
      ~finally:(fun () -> Unix.close out; Unix.close null)
      (fun () -> Unix.create_process argv.(0) argv null out out)
  with
  | _pid -> Ok ()
  | exception Unix.Unix_error (e, _, _) -> Error (Printf.sprintf "could not start wine (%s). see %s" (Unix.error_message e) log)

let helper_exe () =
  let path =
    match Sys.getenv_opt "MOTH_HELPER" with
    | Some p when p <> "" -> if Filename.is_relative p then Filename.concat (Sys.getcwd ()) p else p
    | _ ->
        let exe = try Unix.realpath Sys.executable_name with Unix.Unix_error _ -> Sys.executable_name in
        Filename.concat (Filename.dirname exe) "moth-helper.exe"
  in
  if Sys.file_exists path then Ok path else Error (Printf.sprintf "the helper isn't built (%s not found). run make helper, or set MOTH_HELPER." path)
