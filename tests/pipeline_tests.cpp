#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "book/order_book.hpp"
#include "feed/pipeline.hpp"

using namespace hftob;

namespace {

// --- minimal ITCH 5.0 byte-stream builder (big-endian, type byte at offset 0) -

void put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 24));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
void put48(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int s = 40; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>(v >> s));
}
void put64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>(v >> s));
}
void header(std::vector<std::uint8_t>& b, char type, std::uint64_t ts) {
    b.push_back(static_cast<std::uint8_t>(type));
    put16(b, 1);     // stock locate
    put16(b, 0);     // tracking number
    put48(b, ts);    // 6-byte timestamp
}
void put_stock(std::vector<std::uint8_t>& b) {
    const char sym[8] = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(sym[i]));
}

void add_order(std::vector<std::uint8_t>& b, std::uint64_t ref, char side,
               std::uint32_t shares, Price price) {
    header(b, 'A', 1000);
    put64(b, ref);                              // ref @11
    b.push_back(static_cast<std::uint8_t>(side));  // side @19
    put32(b, shares);                           // shares @20
    put_stock(b);                               // stock @24
    put32(b, static_cast<std::uint32_t>(price));   // price @32 -> len 36
}
void order_executed(std::vector<std::uint8_t>& b, std::uint64_t ref, std::uint32_t shares) {
    header(b, 'E', 1001);
    put64(b, ref);       // @11
    put32(b, shares);    // @19
    put64(b, 7);         // match number @23 -> len 31
}
void order_cancel(std::vector<std::uint8_t>& b, std::uint64_t ref, std::uint32_t shares) {
    header(b, 'X', 1002);
    put64(b, ref);       // @11
    put32(b, shares);    // @19 -> len 23
}
void order_delete(std::vector<std::uint8_t>& b, std::uint64_t ref) {
    header(b, 'D', 1003);
    put64(b, ref);       // @11 -> len 19
}
void order_replace(std::vector<std::uint8_t>& b, std::uint64_t oref, std::uint64_t nref,
                   std::uint32_t shares, Price price) {
    header(b, 'U', 1004);
    put64(b, oref);                              // @11
    put64(b, nref);                              // @19
    put32(b, shares);                            // @27
    put32(b, static_cast<std::uint32_t>(price));    // @31 -> len 35
}

}  // namespace

TEST_CASE("for_each_message walks a back-to-back stream by type length", "[pipeline]") {
    std::vector<std::uint8_t> s;
    add_order(s, 1, 'B', 100, 1000000);
    order_executed(s, 1, 40);
    order_delete(s, 1);

    int count = 0;
    const std::size_t consumed = for_each_message(s.data(), s.size(),
                                                  [&](const itch::Message&) { ++count; });
    CHECK(count == 3);
    CHECK(consumed == s.size());  // whole buffer consumed, nothing left over
}

TEST_CASE("pipelined replay matches single-threaded replay", "[pipeline][thread]") {
    std::vector<std::uint8_t> s;

    // A deterministic opening book...
    add_order(s, 1, 'B', 100, 1000000);
    add_order(s, 2, 'B', 200, 999900);
    add_order(s, 3, 'S', 150, 1000100);
    add_order(s, 4, 'S', 50, 1000200);
    add_order(s, 5, 'B', 300, 1000000);     // aggregate at the top bid
    order_executed(s, 1, 40);               // partial fill ref 1
    order_cancel(s, 2, 50);                 // partial cancel ref 2
    order_delete(s, 4);                     // pull the far ask
    order_replace(s, 3, 6, 120, 1000050);   // reprice ask 3 -> tighter

    // ...then a long tail so the ring actually fills and back-pressures.
    std::uint64_t ref = 1000;
    for (int i = 0; i < 5000; ++i) {
        add_order(s, ref, (i & 1) ? 'S' : 'B',
                  static_cast<std::uint32_t>(10 + (i % 7)),
                  1000000 + ((i % 11) - 5) * 100);
        if (i % 3 == 0) order_executed(s, ref, 3);
        if (i % 5 == 0) order_delete(s, ref);
        ++ref;
    }

    OrderBook a, b;
    const std::size_t na = replay_single_threaded(s.data(), s.size(), a);
    const std::size_t nb = replay_pipelined(s.data(), s.size(), b, /*ring_capacity=*/256);

    CHECK(na == nb);

    Price pa = 0, pb = 0;
    Qty   qa = 0, qb = 0;

    const bool ha = a.best_bid(pa, qa);
    const bool hb = b.best_bid(pb, qb);
    CHECK(ha == hb);
    if (ha && hb) { CHECK(pa == pb); CHECK(qa == qb); }

    const bool sa = a.best_ask(pa, qa);
    const bool sb = b.best_ask(pb, qb);
    CHECK(sa == sb);
    if (sa && sb) { CHECK(pa == pb); CHECK(qa == qb); }

    CHECK(a.order_count() == b.order_count());
    CHECK(a.bid_levels() == b.bid_levels());
    CHECK(a.ask_levels() == b.ask_levels());
}
