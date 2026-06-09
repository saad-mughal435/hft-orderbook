#include <catch2/catch_test_macros.hpp>

#include "core/object_pool.hpp"

using namespace hftob;

TEST_CASE("object pool allocates, stores, frees and reuses handles", "[pool]") {
    ObjectPool<int> pool;

    const auto a = pool.alloc();
    pool[a] = 10;
    const auto b = pool.alloc();
    pool[b] = 20;
    CHECK(pool.live() == 2);
    CHECK(pool[a] == 10);
    CHECK(pool[b] == 20);

    pool.free(a);
    CHECK(pool.live() == 1);

    const auto c = pool.alloc();   // reuses a's freed slot
    CHECK(c == a);
    CHECK(pool.capacity() == 2);   // no new slot was grown
    CHECK(pool.live() == 2);
}

TEST_CASE("object pool reserve avoids growth but creates no live objects", "[pool]") {
    ObjectPool<int> pool(128);
    CHECK(pool.live() == 0);
    CHECK(pool.capacity() == 0);   // reserve != constructed objects

    for (int i = 0; i < 100; ++i) pool.alloc();
    CHECK(pool.live() == 100);
    CHECK(pool.capacity() == 100);
}
