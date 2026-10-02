cmake_minimum_required(VERSION 3.20)
project(nanologger LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

add_library(nanologger INTERFACE)
target_include_directories(nanologger INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(nanologger INTERFACE cxx_std_20)

function(nano_target name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE nanologger Threads::Threads)
  if(NOT MSVC)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.16.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

enable_testing()
nano_target(nanologger_tests tests/test_nanologger.cpp)
target_link_libraries(nanologger_tests PRIVATE Catch2::Catch2WithMain)
add_test(NAME nanologger_tests COMMAND nanologger_tests)

nano_target(nanologger_bench bench/benchmark.cpp)
nano_target(nanolog_decode tools/nanolog_decode.cpp)
