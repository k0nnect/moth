cmake_minimum_required(VERSION 3.20)
project(moth_helper CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)
include_directories(${CMAKE_SOURCE_DIR}/../third_party ${CMAKE_SOURCE_DIR})

# portable engine sources, no OS headers
add_library(moth_engine STATIC engine/pred.cpp engine/block_scan.cpp engine/posset.cpp engine/engine.cpp)

if(MINGW)
  # Windows helper: sources arrive with the win/ tasks
  link_libraries(-static)
else()
  enable_testing()
  add_executable(moth_tests test/test_main.cpp test/test_sanity.cpp test/test_block_scan.cpp
                 test/test_engine_scan.cpp)
  target_link_libraries(moth_tests PRIVATE moth_engine)
  add_test(NAME moth_tests COMMAND moth_tests)
endif()
