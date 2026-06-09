#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "book/book_set.hpp"
#include "book/order_book.hpp"
#include "core/types.hpp"
#include "feed/framing.hpp"
#include "feed/sharded_pipeline.hpp"
#include "feed/synthetic.hpp"
#include "itch/messages.hpp"

using namespace hftob;

TEST_CASE("sharded replay matches single-threaded BookSet across worker counts",
          "[sharded][thread]") {
    const std::vector<std::uint8_t> data = make_synthetic_multi(30000, 8, 99);

    BookSet single;
    for_each_framed_message(data.data(), data.size(),
                            [&](const itch::Message& m) { single.apply(m); });
    REQUIRE(single.book_count() == 8);

    for (unsigned W : {1u, 2u, 3u, 8u}) {
        BookSet           sharded;
        const std::size_t n = replay_sharded(data.data(), data.size(), sharded, W);
        CHECK(n > 0);
        CHECK(sharded.book_count() == single.book_count());
        CHECK(sharded.total_orders() == single.total_orders());

        // Every symbol's reconstructed book must be identical to the single-threaded one.
        for (const auto& kv : single.books()) {
            const std::uint16_t loc = kv.first;
            const OrderBook*    a   = single.book(loc);
            const OrderBook*    b   = sharded.book(loc);
            REQUIRE(b != nullptr);
            CHECK(single.symbol(loc) == sharded.symbol(loc));
            CHECK(a->order_count() == b->order_count());
            CHECK(b->invariant_ok());
            CHECK(a->bids(1000) == b->bids(1000));
            CHECK(a->asks(1000) == b->asks(1000));
        }
    }
}
