#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>
#include <utility>
#include <vector>

#include "core/types.hpp"
#include "mt5/protocol.hpp"

using namespace hftob;
using namespace hftob::mt5;
using Catch::Matchers::WithinAbs;

TEST_CASE("tick round-trips through encode/parse", "[mt5][codec]") {
    Tick in;
    in.symbol = "EURUSD";
    in.time   = 1700000000;
    in.bid    = 1.08523;
    in.ask    = 1.08525;
    in.last   = 1.08524;
    in.volume = 42;

    const std::string line = encode(in);
    CHECK(line.back() == '\n');                  // NDJSON framing
    CHECK(kind_of(line) == MsgKind::Tick);

    Tick out;
    REQUIRE(parse_tick(line, out));
    CHECK(out.symbol == "EURUSD");
    CHECK(out.time == 1700000000u);
    CHECK(out.volume == 42u);
    CHECK_THAT(out.bid, WithinAbs(1.08523, 1e-9));
    CHECK_THAT(out.ask, WithinAbs(1.08525, 1e-9));
    CHECK_THAT(out.last, WithinAbs(1.08524, 1e-9));
}

TEST_CASE("order round-trips and preserves side + correlation id", "[mt5][codec]") {
    Order in;
    in.id     = 7;
    in.symbol = "GBPUSD";
    in.side   = Side::Sell;
    in.volume = 0.25;
    in.price  = 0.0;
    in.kind   = "market";

    const std::string line = encode(in);
    CHECK(kind_of(line) == MsgKind::Order);

    Order out;
    REQUIRE(parse_order(line, out));
    CHECK(out.id == 7u);
    CHECK(out.symbol == "GBPUSD");
    CHECK(out.side == Side::Sell);
    CHECK(out.kind == "market");
    CHECK_THAT(out.volume, WithinAbs(0.25, 1e-9));
}

TEST_CASE("ack round-trips with retcode and ok flag", "[mt5][codec]") {
    Ack in;
    in.id      = 7;
    in.ok      = true;
    in.retcode = 10009;  // TRADE_RETCODE_DONE
    in.message = "done";

    const std::string line = encode(in);
    CHECK(kind_of(line) == MsgKind::Ack);

    Ack out;
    REQUIRE(parse_ack(line, out));
    CHECK(out.id == 7u);
    CHECK(out.ok == true);
    CHECK(out.retcode == 10009);
    CHECK(out.message == "done");
}

TEST_CASE("session-control frames classify correctly", "[mt5][codec]") {
    CHECK(kind_of(encode_hello("ITCHBridge.mq5", 12345)) == MsgKind::Hello);
    CHECK(kind_of(encode_hello_reply("mt5d", true)) == MsgKind::Hello);
    CHECK(kind_of(encode_subscribe("EURUSD")) == MsgKind::Subscribe);
    CHECK(kind_of(encode_nop()) == MsgKind::Nop);
    CHECK(kind_of(encode_bye()) == MsgKind::Bye);
    CHECK(kind_of(encode_heartbeat(123)) == MsgKind::Heartbeat);

    bool ok = false;
    REQUIRE(json_get_bool(encode_hello_reply("mt5d", true), "ok", ok));
    CHECK(ok);
}

TEST_CASE("every frame carries the protocol version", "[mt5][codec]") {
    long long v = 0;
    REQUIRE(json_get_int(encode(Tick{}), "v", v));
    CHECK(v == kProtocolVersion);
}

TEST_CASE("malformed or wrong-type lines are rejected, not misread", "[mt5][codec]") {
    Tick t;
    CHECK_FALSE(parse_tick("not json at all", t));
    CHECK_FALSE(parse_tick("{\"t\":\"tick\",\"v\":1}", t));   // missing symbol/bid/ask
    CHECK(kind_of("{\"t\":\"banana\",\"v\":1}") == MsgKind::Unknown);

    Order o;
    CHECK_FALSE(parse_order("{\"t\":\"order\",\"v\":1,\"symbol\":\"X\"}", o));  // no id/side
}

TEST_CASE("string fields with quotes/backslashes round-trip via escaping", "[mt5][codec]") {
    Ack in;
    in.id      = 9;
    in.ok      = false;
    in.retcode = 10006;
    in.message = "rejected: \"bad\" \\ value";  // embedded quote and backslash

    const std::string line = encode(in);
    Ack out;
    REQUIRE(parse_ack(line, out));
    CHECK(out.message == "rejected: \"bad\" \\ value");
    CHECK(out.id == 9u);
    CHECK(out.retcode == 10006);
}

TEST_CASE("a key name inside a string value is not mistaken for the field", "[mt5][codec]") {
    Order in;
    in.id     = 5;
    in.side   = Side::Buy;
    in.volume = 0.1;
    in.kind   = "market";
    in.symbol = "X,\"id\":999,Y";   // adversarial: looks like an id field, but escaped

    const std::string line = encode(in);
    Order out;
    REQUIRE(parse_order(line, out));
    CHECK(out.id == 5u);             // the real key wins, not the one inside the value
    CHECK(out.symbol == "X,\"id\":999,Y");
}

TEST_CASE("depth snapshot round-trips its ladders", "[mt5][codec]") {
    const std::vector<std::pair<Price, Qty>> bids = {{1000000, 100}, {999900, 200}, {999800, 300}};
    const std::vector<std::pair<Price, Qty>> asks = {{1000100, 150}, {1000200, 250}};

    const std::string line = encode_depth("AAPL", bids, asks);
    CHECK(kind_of(line) == MsgKind::Depth);

    std::string                        sym;
    std::vector<std::pair<Price, Qty>> b, a;
    REQUIRE(parse_depth(line, sym, b, a));
    CHECK(sym == "AAPL");
    CHECK(b == bids);
    CHECK(a == asks);
}

TEST_CASE("empty depth ladders round-trip", "[mt5][codec]") {
    const std::vector<std::pair<Price, Qty>> empty;
    const std::string line = encode_depth("X", empty, empty);

    std::string                        sym;
    std::vector<std::pair<Price, Qty>> b, a;
    REQUIRE(parse_depth(line, sym, b, a));
    CHECK(sym == "X");
    CHECK(b.empty());
    CHECK(a.empty());
}
