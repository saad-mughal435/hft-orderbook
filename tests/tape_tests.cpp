#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <vector>

#include "book/tape.hpp"
#include "core/types.hpp"
#include "feed/framing.hpp"
#include "feed/synthetic.hpp"
#include "itch/decoder.hpp"

using namespace hftob;
using Catch::Matchers::WithinAbs;

TEST_CASE("tape tracks last price, cumulative volume and VWAP", "[tape]") {
    Tape t(1000);
    t.on_trade(1000000, 100, 0);     // $100.00 x100
    t.on_trade(1000200, 200, 100);   // $100.02 x200

    CHECK(t.has_last());
    CHECK(t.last() == 1000200);
    CHECK(t.volume() == 300u);
    CHECK_THAT(t.vwap(), WithinAbs((100.00 * 100 + 100.02 * 200) / 300.0, 1e-6));
}

TEST_CASE("tape forms OHLCV bars per nanosecond bucket", "[tape]") {
    Tape t(1000);                    // 1000 ns bars
    t.on_trade(1000000, 10, 100);    // bucket 0: open
    t.on_trade(1000300, 20, 500);    // bucket 0: high
    t.on_trade(999800, 30, 900);     // bucket 0: low + close
    t.on_trade(1000100, 40, 1500);   // bucket 1 -> closes bucket 0

    const std::vector<Bar> bars = t.bars(/*include_current=*/true);
    REQUIRE(bars.size() == 2);
    CHECK(bars[0].open == 1000000);
    CHECK(bars[0].high == 1000300);
    CHECK(bars[0].low == 999800);
    CHECK(bars[0].close == 999800);
    CHECK(bars[0].volume == 60u);
    CHECK(bars[1].open == 1000100);
    CHECK(bars[1].volume == 40u);
}

TEST_CASE("the synthetic feed now carries trades the tape can consume", "[tape][synthetic]") {
    const std::vector<std::uint8_t> data = make_synthetic_itch(20000, 7);
    Tape        tape(1000);
    std::size_t trades = 0;
    for_each_framed_message(data.data(), data.size(), [&](const itch::Message& m) {
        if (m.type == itch::MsgType::Trade) {
            tape.on_trade(m.price, m.shares, m.timestamp);
            ++trades;
        }
    });
    CHECK(trades > 0);               // P trades are present
    CHECK(tape.has_last());
    CHECK(tape.volume() > 0u);
    CHECK(tape.bar_count() >= 1);
}
