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
    tests/spsc_tests.cpp
    tests/pipeline_tests.cpp
  )
  target_link_libraries(hftob_tests PRIVATE hftob Catch2::Catch2WithMain Threads::Threads)

  enable_testing()
  add_test(NAME unit COMMAND hftob_tests)
endif()
