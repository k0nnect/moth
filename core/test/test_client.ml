open Moth_core

let check_res n w g = Alcotest.(check (result string string)) n w g

let actor_of_env () =
  check_res "unset" (Ok "user") (Client.actor_of_env None);
  check_res "empty" (Ok "user") (Client.actor_of_env (Some ""));
  check_res "user" (Ok "user") (Client.actor_of_env (Some "user"));
  check_res "agent" (Ok "agent") (Client.actor_of_env (Some "agent"));
  check_res "case matters" (Error "MOTH_ACTOR must be user or agent") (Client.actor_of_env (Some "Agent"));
  check_res "anything else" (Error "MOTH_ACTOR must be user or agent") (Client.actor_of_env (Some "bot"))

let () = Alcotest.run "client" [ ("actor", [ Alcotest.test_case "MOTH_ACTOR" `Quick actor_of_env ]) ]
