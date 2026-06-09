#include <catch2/catch_test_macros.hpp>

#include <functional>

#include "core/types.hpp"
#include "core/windowed_levels.hpp"

using namespace hftob;

TEST_CASE("WindowedLevels keeps bids highest-first and asks lowest-first", "[levels][windowed]") {
    WindowedLevels<std::greater<Price>> bids;
    bids.inc(1000000, 100);
    bids.inc(999900, 200);
    bids.inc(1000100, 50);
    Price p = 0;
    Qty   q = 0;
    REQUIRE(bids.best(p, q));
    CHECK(p == 1000100);
    CHECK(q == 50u);
    CHECK(bids.at(999900) == 200u);
    CHECK(bids.size() == 3);
    const auto tb = bids.top(2);
    REQUIRE(tb.size() == 2);
    CHECK(tb[0].first == 1000100);
    CHECK(tb[1].first == 1000000);

    WindowedLevels<std::less<Price>> asks;
    asks.inc(1000100, 10);
    asks.inc(1000000, 20);
    asks.inc(1000200, 5);
    REQUIRE(asks.best(p, q));
    CHECK(p == 1000000);
    CHECK(q == 20u);
    const auto ta = asks.top(3);
    REQUIRE(ta.size() == 3);
    CHECK(ta[0].first == 1000000);
    CHECK(ta[1].first == 1000100);
    CHECK(ta[2].first == 1000200);
}

TEST_CASE("WindowedLevels surfaces the next-best when the top empties", "[levels][windowed]") {
    WindowedLevels<std::greater<Price>> bids;
    bids.inc(1000000, 100);
    bids.inc(1000100, 50);   // best
    bids.inc(999900, 200);
    Price p = 0;
    Qty   q = 0;
    REQUIRE(bids.best(p, q));
    CHECK(p == 1000100);
    bids.dec(1000100, 50);
    REQUIRE(bids.best(p, q));
    CHECK(p == 1000000);
    CHECK(bids.size() == 2);
    bids.dec(1000000, 100);
    REQUIRE(bids.best(p, q));
    CHECK(p == 999900);
    bids.dec(999900, 200);
    CHECK_FALSE(bids.best(p, q));
    CHECK(bids.size() == 0);
}

TEST_CASE("WindowedLevels spills a far-out level to overflow, still correct", "[levels][windowed]") {
    WindowedLevels<std::greater<Price>> bids;
    bids.inc(1000000, 100);
    const Price far = 1000000 - 200000;   // far below the window -> overflow (worse for a bid)
    bids.inc(far, 300);

    CHECK(bids.size() == 2);
    CHECK(bids.at(far) == 300u);
    Price p = 0;
    Qty   q = 0;
    REQUIRE(bids.best(p, q));
    CHECK(p == 1000000);                  // window level is still best
    const auto top = bids.top(2);
    REQUIRE(top.size() == 2);
    CHECK(top[0].first == 1000000);
    CHECK(top[1].first == far);           // overflow appears after the window
    bids.dec(far, 300);
    CHECK(bids.size() == 1);
    CHECK(bids.at(far) == 0u);
}

TEST_CASE("WindowedLevels re-centres on a better far level, preserving all levels",
          "[levels][windowed]") {
    WindowedLevels<std::greater<Price>> bids;
    bids.inc(1000000, 100);
    bids.inc(999900, 200);
    const Price hi = 1000000 + 200000;    // far above the window -> triggers a re-centre
    bids.inc(hi, 400);

    CHECK(bids.size() == 3);
    Price p = 0;
    Qty   q = 0;
    REQUIRE(bids.best(p, q));
    CHECK(p == hi);
    CHECK(q == 400u);
    CHECK(bids.at(1000000) == 100u);      // old levels survive the re-centre
    CHECK(bids.at(999900) == 200u);
    const auto top = bids.top(3);
    REQUIRE(top.size() == 3);
    CHECK(top[0].first == hi);
    CHECK(top[1].first == 1000000);
    CHECK(top[2].first == 999900);
}
