let env name = match Sys.getenv_opt name with Some "" | None -> None | Some v -> Some v
let home () = Option.value (env "HOME") ~default:"/"
let is_dir p = try Sys.is_directory p with Sys_error _ -> false

let find_app () =
  let not_found = Error "crossover not found. set MOTH_CROSSOVER to the path of CrossOver.app." in
  match env "MOTH_CROSSOVER" with
  | Some p -> if is_dir p then Ok p else not_found
  | None -> (
      let candidates = [ Filename.concat (home ()) "Applications/CrossOver.app"; "/Applications/CrossOver.app" ] in
      match List.find_opt is_dir candidates with Some p -> Ok p | None -> not_found)

let bottles_dir () =
  match env "MOTH_BOTTLES" with Some d -> d | None -> Filename.concat (home ()) "Library/Application Support/CrossOver/Bottles"

let list_bottles () =
  let dir = bottles_dir () in
  match Sys.readdir dir with
  | exception Sys_error _ -> []
  | names ->
      Array.to_list names
      |> List.filter (fun n -> Sys.file_exists (Filename.concat (Filename.concat dir n) "cxbottle.conf"))
      |> List.sort compare

let check_bottle name =
  let bottles = list_bottles () in
  if List.mem name bottles then Ok ()
  else Error (Printf.sprintf "no bottle called \"%s\". bottles: %s" name (String.concat ", " bottles))

let wine_argv ~app ~bottle ~exe args =
  Array.of_list
    ([ app ^ "/Contents/SharedSupport/CrossOver/bin/wine"; "--bottle"; bottle; "--workdir"; Filename.dirname exe; exe ] @ args)
