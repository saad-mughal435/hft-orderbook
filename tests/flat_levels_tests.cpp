#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <vector>

#include "book/order_book.hpp"
#include "core/flat_levels.hpp"
#include "core/types.hpp"
#include "feed/framing.hpp"
#include "feed/synthetic.hpp"

using namespace hftob;

TEST_CASE("FlatLevels keeps bids highest-first and asks lowest-first", "[levels][flat]") {
    FlatLevels<std::greater<Price>> bids;
    bids.inc(1000000, 100);
    bids.inc(999900, 200);
    bids.inc(1000100, 50);
    Price p = 0;
    Qty   q = 0;
    REQUIRE(bids.best(p, q));
    CHECK(p == 1000100);   // highest bid first
    CHECK(q == 50u);
    CHECK(bids.at(999900) == 200u);
    CHECK(bids.size() == 3);
    bids.dec(1000100, 50);
    REQUIRE(bids.best(p, q));
    CHECK(p == 1000000);   // top removed -> next-best surfaces

    FlatLevels<std::less<Price>> asks;
    asks.inc(1000100, 10);
    asks.inc(1000000, 20);
    asks.inc(1000200, 5);
    REQUIRE(asks.best(p, q));
    CHECK(p == 1000000);   // lowest ask first
    CHECK(q == 20u);
    const auto top = asks.top(2);
    REQUIRE(top.size() == 2);
    CHECK(top[0].first == 1000000);
    CHECK(top[1].first == 1000100);
}

TEST_CASE("FlatOrderBook reconstructs identically to the std::map OrderBook",
          "[book][flat][parity]") {
    const std::vector<std::uint8_t> data = make_synthetic_itch(20000, 4242);

    OrderBook     a;
    FlatOrderBook b;
    for_each_framed_message(data.data(), data.size(),
                            [&](const itch::Message& m) { a.apply(m); });
    for_each_framed_message(data.data(), data.size(),
                            [&](const itch::Message& m) { b.apply(m); });

    CHECK(a.invariant_ok());
    CHECK(b.invariant_ok());
    CHECK(a.order_count() == b.order_count());
    CHECK(a.bid_levels() == b.bid_levels());
    CHECK(a.ask_levels() == b.ask_levels());

    Price pa = 0, pb = 0;
    Qty   qa = 0, qb = 0;
    const bool hba = a.best_bid(pa, qa);
    const bool hbb = b.best_bid(pb, qb);
    CHECK(hba == hbb);
    if (hba && hbb) { CHECK(pa == pb); CHECK(qa == qb); }

    const bool haa = a.best_ask(pa, qa);
    const bool hab = b.best_ask(pb, qb);
    CHECK(haa == hab);
    if (haa && hab) { CHECK(pa == pb); CHECK(qa == qb); }

    // Full ladders must be element-for-element identical.
    CHECK(a.bids(1000000) == b.bids(1000000));
    CHECK(a.asks(1000000) == b.asks(1000000));
}
