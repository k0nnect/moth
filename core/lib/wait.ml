type event = { cmd_line : string; ok : bool; summary : string }

type t = {
  mutable cursor : int;
  mutable quiet : bool;
  mutable held : event list; (* newest first *)
  mutable first : float; (* when the oldest held event was seen *)
  mutable last : float; (* when the last line was seen *)
  mutable busy : int; (* busy lines whose event has not come *)
}

type verdict = Keep_waiting | Tell of { notes : string; cursor : int }

let start ~cursor ~quiet = { cursor; quiet; held = []; first = 0.; last = 0.; busy = 0 }

(* a session's cursor file: the seq, and " quiet" when moth was quiet at it (123 quiet) *)
let cursor_text cursor quiet = string_of_int cursor ^ if quiet then " quiet" else ""

let parse_cursor text =
  match String.split_on_char ' ' text with
  | [ n ] -> Option.map (fun n -> (n, false)) (int_of_string_opt n)
  | [ n; "quiet" ] -> Option.map (fun n -> (n, true)) (int_of_string_opt n)
  | _ -> None
let cursor t = t.cursor
let quiet t = t.quiet
let dropped t = t.busy <- 0
let settle_after = 1.5
let longest = 10.
let most_lines = 12

let words ev =
  let open Yojson.Safe.Util in
  to_string (member "cmd" ev) :: List.map to_string (to_list (member "args" ev))

let take_in t ~now ev =
  let open Yojson.Safe.Util in
  match member "seq" ev with
  | `Int seq when seq > t.cursor -> (
      t.cursor <- seq;
      match words ev with
      | "quiet" :: (("on" | "off") as v) :: _ when member "ok" ev = `Bool true ->
          t.quiet <- v = "on";
          (* going quiet drops what was held; the cursor has moved past it *)
          if t.quiet then t.held <- []
      | "quiet" :: _ -> ()
      | _ :: _ as ws when member "actor" ev <> `String "agent" && member "act" ev = `Bool true && not t.quiet ->
          let held =
            {
              cmd_line = String.concat " " ws;
              ok = member "ok" ev = `Bool true;
              summary = (match member "summary" ev with `String s -> s | _ -> "");
            }
          in
          if t.held = [] then t.first <- now;
          t.held <- held :: t.held
      | _ -> ())
  | _ -> ()

let line t ~now j =
  let open Yojson.Safe.Util in
  t.last <- now;
  try
    match (member "busy" j, member "event" j) with
    | `Assoc _, _ -> t.busy <- t.busy + 1
    | _, (`Assoc _ as ev) ->
        t.busy <- max 0 (t.busy - 1);
        take_in t ~now ev
    | _ -> ()
  with Type_error _ -> ()

let check t ~now =
  if t.held = [] || t.busy > 0 || not (now -. t.last >= settle_after || now -. t.first >= longest) then Keep_waiting
  else begin
    let held = List.rev t.held in
    t.held <- [];
    let n = List.length held in
    let shown = if n > most_lines then List.filteri (fun i _ -> i >= n - most_lines) held else held in
    let one e = Printf.sprintf "- %s: %s%s" e.cmd_line (if e.ok then "" else "failed: ") e.summary in
    let earlier = if n > most_lines then [ Printf.sprintf "- (%d earlier steps: moth log)" (n - most_lines) ] else [] in
    Tell { notes = String.concat "\n" (("[moth] the person did this in moth:" :: earlier) @ List.map one shown); cursor = t.cursor }
  end
