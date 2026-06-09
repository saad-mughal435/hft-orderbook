#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "core/cpu.hpp"

using namespace hftob;

TEST_CASE("cpu_relax and rdtsc run without crashing", "[cpu]") {
    for (int i = 0; i < 256; ++i) cpu_relax();   // a safe spin-pause hint

    const std::uint64_t  a    = rdtsc();
    volatile std::uint64_t sink = 0;
    for (int i = 0; i < 5000; ++i) {
        sink += static_cast<std::uint64_t>(i);
        cpu_relax();
    }
    const std::uint64_t b = rdtsc();
    if (a != 0 || b != 0) CHECK(b >= a);          // x86: TSC is monotonic; else both 0
    CHECK(sink > 0);
}

TEST_CASE("pin_this_thread is callable", "[cpu]") {
    const bool pinned = pin_this_thread(0);       // may be denied in a restricted sandbox
    (void)pinned;
    SUCCEED();
}
