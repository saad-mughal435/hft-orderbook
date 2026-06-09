cmake_minimum_required(VERSION 3.16)
project(hft_orderbook VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

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
  )
  target_link_libraries(hftob_tests PRIVATE hftob Catch2::Catch2WithMain)

  enable_testing()
  add_test(NAME unit COMMAND hftob_tests)
endif()
