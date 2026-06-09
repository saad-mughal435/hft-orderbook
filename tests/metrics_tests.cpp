#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "book/metrics.hpp"
#include "book/order_book.hpp"
#include "core/types.hpp"

using namespace hftob;
using Catch::Matchers::WithinAbs;

TEST_CASE("metrics on a two-sided book", "[metrics]") {
    OrderBook b;
    b.add(1, Side::Buy, 1000000, 100);   // bid $100.00 x100
    b.add(2, Side::Buy, 999900, 200);    //     $99.99  x200
    b.add(3, Side::Sell, 1000100, 50);   // ask $100.01 x50
    b.add(4, Side::Sell, 1000200, 150);  //     $100.02 x150

    const BookMetrics m = compute_metrics(b, 5);
    REQUIRE(m.two_sided);
    CHECK(m.bid_px == 1000000);
    CHECK(m.ask_px == 1000100);
    CHECK(m.bid_qty == 100u);
    CHECK(m.ask_qty == 50u);

    CHECK_THAT(m.mid, WithinAbs(100.005, 1e-9));
    CHECK(m.spread_ticks == 100);  // one cent = 100 price ticks
    CHECK_THAT(m.spread_bps, WithinAbs((0.01 / 100.005) * 1e4, 1e-6));

    // micro-price weights each side's price by the opposite side's size
    CHECK_THAT(m.microprice, WithinAbs((100.00 * 50 + 100.01 * 100) / 150.0, 1e-6));

    CHECK_THAT(m.imbalance_top, WithinAbs(50.0 / 150.0, 1e-9));   // (100-50)/150
    CHECK(m.bid_depth_n == 300u);                                  // 100 + 200
    CHECK(m.ask_depth_n == 200u);                                  // 50 + 150
    CHECK_THAT(m.imbalance_n, WithinAbs(100.0 / 500.0, 1e-9));     // (300-200)/500
}

TEST_CASE("metrics on empty and one-sided books are guarded", "[metrics]") {
    OrderBook b;
    const BookMetrics m0 = compute_metrics(b);
    CHECK_FALSE(m0.two_sided);
    CHECK_FALSE(m0.has_bid);
    CHECK_FALSE(m0.has_ask);

    b.add(1, Side::Buy, 1000000, 100);
    const BookMetrics m1 = compute_metrics(b);
    CHECK(m1.has_bid);
    CHECK_FALSE(m1.has_ask);
    CHECK_FALSE(m1.two_sided);
    CHECK(m1.bid_px == 1000000);
}
