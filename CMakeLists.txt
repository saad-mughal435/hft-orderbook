cmake_minimum_required(VERSION 3.16)
project(hft_orderbook VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# ---- engine library -------------------------------------------------------
add_library(hftob
  src/itch/decoder.cpp
)
target_include_directories(hftob PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
  target_compile_options(hftob PRIVATE /W4)
else()
  target_compile_options(hftob PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- apps -----------------------------------------------------------------
# obreplay: replay an ITCH capture (or a synthetic stream) through the pipeline.
add_executable(obreplay apps/obreplay.cpp)
target_link_libraries(obreplay PRIVATE hftob Threads::Threads)

# Optional transparent .gz support for real NASDAQ samples.
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
  target_compile_definitions(obreplay PRIVATE HFTOB_HAVE_ZLIB)
  target_link_libraries(obreplay PRIVATE ZLIB::ZLIB)
  message(STATUS "obreplay: zlib found, .gz captures supported")
else()
  message(STATUS "obreplay: zlib not found, .gz captures disabled (uncompressed only)")
endif()

# gencap: write a small reproducible synthetic capture to disk.
add_executable(gencap apps/gencap.cpp)
target_link_libraries(gencap PRIVATE hftob)

# mt5d / wsbook: POSIX-socket servers (Linux/macOS only).
if(UNIX)
  add_executable(mt5d apps/mt5d.cpp)
  target_link_libraries(mt5d PRIVATE hftob Threads::Threads)

  add_executable(wsbook apps/wsbook.cpp)
  target_link_libraries(wsbook PRIVATE hftob Threads::Threads)
endif()

# ---- benchmarks (opt-in: -DHFTOB_BUILD_BENCH=ON) --------------------------
option(HFTOB_BUILD_BENCH "Build Google Benchmark microbenchmarks" OFF)
if(HFTOB_BUILD_BENCH)
  include(FetchContent)
  set(BENCHMARK_ENABLE_TESTING      OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS  OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL      OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR       OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.8.3
  )
  FetchContent_MakeAvailable(benchmark)

  add_executable(hftob_bench bench/bench.cpp)
  target_link_libraries(hftob_bench PRIVATE hftob benchmark::benchmark Threads::Threads)
endif()

# ---- fuzzing (opt-in: -DHFTOB_BUILD_FUZZ=ON, clang only) ------------------
option(HFTOB_BUILD_FUZZ "Build the libFuzzer decode harness (clang only)" OFF)
if(HFTOB_BUILD_FUZZ)
  add_executable(decode_fuzz fuzz/decode_fuzz.cpp)
  target_link_libraries(decode_fuzz PRIVATE hftob)
  target_compile_options(decode_fuzz PRIVATE -g -O1 -fsanitize=fuzzer,address)
  target_link_options(decode_fuzz PRIVATE -fsanitize=fuzzer,address)
endif()

# ---- tests ----------------------------------------------------------------
option(HFTOB_BUILD_TESTS "Build unit tests" ON)
if(HFTOB_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
  )
  FetchContent_MakeAvailable(Catch2)

  add_executable(hftob_tests
    tests/itch_tests.cpp
    tests/book_tests.cpp
    tests/book_props_tests.cpp
    tests/metrics_tests.cpp
    tests/tape_tests.cpp
    tests/object_pool_tests.cpp
    tests/flat_levels_tests.cpp
    tests/windowed_levels_tests.cpp
    tests/sha1_tests.cpp
    tests/spsc_tests.cpp
    tests/pipeline_tests.cpp
    tests/sharded_tests.cpp
    tests/mt5_codec_tests.cpp
  )
  target_link_libraries(hftob_tests PRIVATE hftob Catch2::Catch2WithMain Threads::Threads)

  enable_testing()
  add_test(NAME unit COMMAND hftob_tests)

  # Loopback-socket integration tests (POSIX only).
  if(UNIX)
    add_executable(mt5_itest tests/mt5_itest.cpp)
    target_link_libraries(mt5_itest PRIVATE hftob Catch2::Catch2WithMain Threads::Threads)
    add_test(NAME mt5_integration COMMAND mt5_itest)

    add_executable(ws_itest tests/ws_itest.cpp)
    target_link_libraries(ws_itest PRIVATE hftob Catch2::Catch2WithMain Threads::Threads)
    add_test(NAME ws_integration COMMAND ws_itest)
  endif()
endif()
