#include <catch2/catch_test_macros.hpp>

#include <thread>

#include "core/spsc_ring.hpp"

using namespace hftob;

TEST_CASE("capacity rounds up to a power of two (min 2)", "[spsc]") {
    CHECK(SpscRing<int>(0).capacity() == 2u);
    CHECK(SpscRing<int>(1).capacity() == 2u);
    CHECK(SpscRing<int>(2).capacity() == 2u);
    CHECK(SpscRing<int>(3).capacity() == 4u);
    CHECK(SpscRing<int>(5).capacity() == 8u);
    CHECK(SpscRing<int>(1000).capacity() == 1024u);
}

TEST_CASE("push/pop preserve FIFO order", "[spsc]") {
    SpscRing<int> r(8);
    for (int i = 0; i < 5; ++i) CHECK(r.push(i));
    int v = -1;
    for (int i = 0; i < 5; ++i) {
        REQUIRE(r.pop(v));
        CHECK(v == i);
    }
    CHECK_FALSE(r.pop(v));  // drained
}

TEST_CASE("pop on an empty ring returns false and leaves out untouched", "[spsc]") {
    SpscRing<int> r(4);
    int v = 42;
    CHECK_FALSE(r.pop(v));
    CHECK(v == 42);
}

TEST_CASE("push on a full ring returns false; freeing a slot lets it resume", "[spsc]") {
    SpscRing<int> r(4);  // capacity == 4
    for (int i = 0; i < 4; ++i) CHECK(r.push(i));
    CHECK(r.size_approx() == 4u);
    CHECK_FALSE(r.push(99));  // full
    int v = 0;
    REQUIRE(r.pop(v));
    CHECK(v == 0);
    CHECK(r.push(99));        // one slot freed -> succeeds
}

TEST_CASE("interleaved push/pop wraps the index correctly", "[spsc]") {
    SpscRing<int> r(4);
    int v = 0;
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(r.push(i));
        REQUIRE(r.pop(v));
        CHECK(v == i);
    }
    CHECK(r.empty_approx());
}

TEST_CASE("concurrent producer/consumer transfer every item in order", "[spsc][thread]") {
    constexpr int N = 200000;
    SpscRing<int> r(1024);

    std::thread producer([&] {
        for (int i = 0; i < N; ++i)
            while (!r.push(i)) std::this_thread::yield();
    });

    long long sum     = 0;
    int       expected = 0;
    int       received = 0;
    bool      ordered  = true;
    int       v        = 0;
    while (received < N) {
        if (r.pop(v)) {
            if (v != expected) ordered = false;  // SPSC must be strictly FIFO
            ++expected;
            sum += v;
            ++received;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();

    CHECK(ordered);
    CHECK(sum == static_cast<long long>(N - 1) * N / 2);
}
