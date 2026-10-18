cmake_minimum_required(VERSION 3.20)
project(itouKV VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
add_library(itoukv STATIC src/store.cpp)
target_include_directories(itoukv PUBLIC include)
target_link_libraries(itoukv PUBLIC Threads::Threads)
option(ITOUKV_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
if(ITOUKV_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "ITOUKV_SANITIZE requires GCC or Clang")
  endif()
  target_compile_options(itoukv PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(itoukv PUBLIC -fsanitize=address,undefined)
endif()
if(MSVC)
  target_compile_options(itoukv PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(itoukv PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(itoukv_cli src/cli.cpp)
target_link_libraries(itoukv_cli PRIVATE itoukv)
set_target_properties(itoukv_cli PROPERTIES OUTPUT_NAME itoukv)

add_executable(itoukv_notes examples/notes.cpp)
target_link_libraries(itoukv_notes PRIVATE itoukv)

include(CTest)
if(BUILD_TESTING)
  add_executable(itoukv_tests tests/storage_test.cpp)
  target_link_libraries(itoukv_tests PRIVATE itoukv)
  add_test(NAME storage COMMAND itoukv_tests)
  find_program(NODE_EXECUTABLE node)
  if(NODE_EXECUTABLE)
    add_test(NAME process_recovery
      COMMAND ${NODE_EXECUTABLE} --test ${CMAKE_CURRENT_SOURCE_DIR}/tests/process.test.mjs)
    set_tests_properties(process_recovery PROPERTIES ENVIRONMENT
      "ITOUKV_CLI=$<TARGET_FILE:itoukv_cli>;ITOUKV_TEST_HELPER=$<TARGET_FILE:itoukv_tests>")
    add_test(NAME notes_cli
      COMMAND ${NODE_EXECUTABLE} --test ${CMAKE_CURRENT_SOURCE_DIR}/tests/notes.test.mjs)
    set_tests_properties(notes_cli PROPERTIES ENVIRONMENT
      "ITOUKV_NOTES=$<TARGET_FILE:itoukv_notes>")
  endif()
endif()

add_executable(itoukv_bench bench/benchmark.cpp)
target_link_libraries(itoukv_bench PRIVATE itoukv)
add_executable(itoukv_batch_bench bench/batch_benchmark.cpp)
target_link_libraries(itoukv_batch_bench PRIVATE itoukv)
