cmake_minimum_required(VERSION 3.20)
project(moth_app CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for an editor: see .clangd

add_compile_options(-Wall -Wextra)

# qtbase only, linked dynamically; make passes -DCMAKE_PREFIX_PATH=$(brew --prefix qtbase)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Test)

# the window's code without main(), which the program and every test link: one line per source file
add_library(moth_window STATIC
  src/paths.cpp
  src/core_client.cpp
  src/board.cpp
  src/main_window.cpp
  src/game_bar.cpp
  src/search_bar.cpp
  src/results_model.cpp
  src/results_panel.cpp
  src/kept_panel.cpp
)
target_include_directories(moth_window PUBLIC src)
target_link_libraries(moth_window PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

add_executable(moth-app src/main.cpp)
target_link_libraries(moth-app PRIVATE moth_window)

# tests: each test/<name>.cpp is a program and a ctest test, run offscreen against the fake core.
# A new test is one more name in the list.
enable_testing()
add_library(moth_fake_core STATIC test/fake_core.cpp)
target_include_directories(moth_fake_core PUBLIC test)
target_link_libraries(moth_fake_core PUBLIC moth_window Qt6::Test)

set(MOTH_APP_TESTS
  test_core_client
  test_board
  test_main_window
  test_game_bar
  test_search_bar
  test_results
  test_kept_panel
)
foreach(name IN LISTS MOTH_APP_TESTS)
  add_executable(${name} test/${name}.cpp)
  target_link_libraries(${name} PRIVATE moth_fake_core)
  add_test(NAME ${name} COMMAND ${name})
  # offscreen windows; the font-alias timing note that platform prints now and then is not ours
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=qt.qpa.fonts.warning=false")
endforeach()
