cmake_minimum_required(VERSION 3.20)
project(moth_helper CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)
include_directories(${CMAKE_SOURCE_DIR}/../third_party ${CMAKE_SOURCE_DIR})

find_package(Threads REQUIRED)

# portable engine sources, no OS headers; scans run on std::thread workers
add_library(moth_engine STATIC engine/pred.cpp engine/block_scan.cpp engine/posset.cpp engine/engine.cpp)
target_link_libraries(moth_engine PUBLIC Threads::Threads)

# JSON protocol handler, also portable; the Windows helper links it with moth_engine
add_library(moth_proto STATIC proto/handler.cpp)
target_link_libraries(moth_proto PUBLIC moth_engine)

if(MINGW)
  link_libraries(-static)
  set(MOTH_BIN ${CMAKE_SOURCE_DIR}/../bin)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${MOTH_BIN})
  # the helper ships as x86_64 only; the i686 toolchain builds just the 32-bit stand-in target
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(moth-helper win/main.cpp win/win_memory.cpp win/win_platform.cpp)
    target_link_libraries(moth-helper PRIVATE moth_proto ws2_32)
    add_executable(moth-target ../tests/target/target.cpp)
    target_link_libraries(moth-target PRIVATE ws2_32)
  else()
    add_executable(moth-target32 ../tests/target/target.cpp)
    target_link_libraries(moth-target32 PRIVATE ws2_32)
  endif()
else()
  enable_testing()
  add_executable(moth_tests test/test_main.cpp test/test_sanity.cpp test/test_block_scan.cpp
                 test/test_engine_scan.cpp test/test_engine_narrow.cpp test/test_engine_parallel.cpp test/test_handler.cpp)
  target_link_libraries(moth_tests PRIVATE moth_proto)
  add_test(NAME moth_tests COMMAND moth_tests)

  add_executable(moth_bench test/bench.cpp)
  target_link_libraries(moth_bench PRIVATE moth_engine)
endif()
