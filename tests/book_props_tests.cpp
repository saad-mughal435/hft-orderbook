#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <vector>

#include "book/order_book.hpp"
#include "core/types.hpp"
#include "feed/framing.hpp"
#include "feed/synthetic.hpp"

using namespace hftob;

TEST_CASE("make_synthetic_book reconstructs a never-crossed book", "[book][prop]") {
    const std::vector<std::uint8_t> data = make_synthetic_book(10000, 3);

    OrderBook   book;
    bool        crossed = false;
    std::size_t checks  = 0;
    for_each_framed_message(data.data(), data.size(), [&](const itch::Message& m) {
        book.apply(m);
        Price bp = 0, ap = 0;
        Qty   bq = 0, aq = 0;
        if (book.best_bid(bp, bq) && book.best_ask(ap, aq)) {
            ++checks;
            if (bp >= ap) crossed = true;  // best bid must be strictly below best ask
        }
    });
    CHECK_FALSE(crossed);
    CHECK(checks > 0);
    CHECK(book.invariant_ok());
}

TEST_CASE("level totals stay consistent across a long synthetic replay", "[book][prop]") {
    const std::vector<std::uint8_t> data = make_synthetic_itch(20000, 12345);

    OrderBook   book;
    std::size_t applied      = 0;
    bool        ok_throughout = true;
    for_each_framed_message(data.data(), data.size(), [&](const itch::Message& m) {
        book.apply(m);
        ++applied;
        if ((applied % 1000) == 0 && !book.invariant_ok()) ok_throughout = false;
    });

    CHECK(applied > 0);
    CHECK(ok_throughout);
    CHECK(book.invariant_ok());   // final state is internally consistent
}

TEST_CASE("deleting every resting order empties the book", "[book][prop]") {
    OrderBook b;
    for (OrderRef r = 1; r <= 50; ++r)
        b.add(r, (r & 1) ? Side::Buy : Side::Sell,
              1000000 + static_cast<Price>(r % 10) * 100,
              static_cast<Qty>(10 + r));
    CHECK(b.invariant_ok());

    for (OrderRef r = 1; r <= 50; ++r) CHECK(b.remove(r));

    CHECK(b.order_count() == 0);
    CHECK(b.bid_levels() == 0);
    CHECK(b.ask_levels() == 0);
    CHECK(b.invariant_ok());
}

TEST_CASE("invariant survives a randomized add/reduce/remove/replace sequence",
          "[book][prop]") {
    OrderBook             b;
    std::mt19937          rng(99);
    std::vector<OrderRef> live;
    OrderRef              next = 1;
    bool                  ok   = true;

    auto rnd_price = [&]() {
        return static_cast<Price>(1000000 + (static_cast<int>(rng() % 21) - 10) * 100);
    };

    for (int i = 0; i < 20000 && ok; ++i) {
        const unsigned roll = rng() % 100;
        if (live.size() < 4 || roll < 50) {                 // add
            const OrderRef r = next++;
            b.add(r, (rng() & 1) ? Side::Buy : Side::Sell, rnd_price(),
                  static_cast<Qty>(1 + rng() % 500));
            live.push_back(r);
        } else if (roll < 70) {                             // reduce (may zero out)
            b.reduce(live[rng() % live.size()], static_cast<Qty>(1 + rng() % 50));
        } else if (roll < 85) {                             // remove
            const std::size_t idx = rng() % live.size();
            b.remove(live[idx]);
            live[idx] = live.back();
            live.pop_back();
        } else {                                            // replace
            const std::size_t idx = rng() % live.size();
            const OrderRef     nr  = next++;
            b.replace(live[idx], nr, static_cast<Qty>(1 + rng() % 500), rnd_price());
            live[idx] = nr;
        }
        if (!b.invariant_ok()) ok = false;
    }
    CHECK(ok);
}
