// Full MT5-bridge integration test over a real loopback TCP socket: a mock EA
// (this test) streams recorded ticks to the bridge server (run_bridge, in a
// second thread), the server's strategy turns some ticks into orders, the mock
// EA executes them by returning acks, and we assert the full round trip — with no
// Windows and no MetaTrader terminal. Linux/macOS only (POSIX sockets).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "book/order_book.hpp"
#include "core/types.hpp"
#include "mt5/protocol.hpp"
#include "mt5/server.hpp"
#include "mt5/strategy.hpp"
#include "mt5/tcp.hpp"

using namespace hftob;
using namespace hftob::mt5;

namespace {

// A deterministic tick tape: a rising ramp (triggers Buys) then a falling ramp
// (triggers Sells), so the ExampleStrategy (>= 0.0010 mid move) fires repeatedly.
std::vector<Tick> recorded_ticks() {
    std::vector<Tick> v;
    double mid = 1.10000;
    for (int i = 0; i < 12; ++i) {            // up ramp: +0.0020 each step
        mid += 0.0020;
        v.push_back(Tick{"EURUSD", static_cast<std::uint64_t>(1700000000 + i),
                         mid - 0.00001, mid + 0.00001, mid, 1});
    }
    for (int i = 0; i < 12; ++i) {            // down ramp: -0.0020 each step
        mid -= 0.0020;
        v.push_back(Tick{"EURUSD", static_cast<std::uint64_t>(1700000100 + i),
                         mid - 0.00001, mid + 0.00001, mid, 1});
    }
    return v;
}

}  // namespace

TEST_CASE("mt5 bridge: ticks in -> orders out -> acks round-trip over TCP",
          "[mt5][itest][thread]") {
    Listener listener(0);                       // OS-assigned ephemeral loopback port
    const std::uint16_t port = listener.port();
    REQUIRE(port != 0);

    BridgeStats server_stats;
    std::thread server([&] {
        LineSocket conn = listener.accept();
        ExampleStrategy strat;                  // default: 0.0010 step, 0.10 lots
        server_stats = run_bridge(conn, strat);
    });

    // ---- mock EA (client) ---------------------------------------------------
    LineSocket client = LineSocket::connect("127.0.0.1", port);

    REQUIRE(client.send_line(encode_hello("ITCHBridge.mq5", 12345)));
    std::string reply;
    REQUIRE(client.recv_line(reply));
    CHECK(kind_of(reply) == MsgKind::Hello);
    bool ok = false;
    REQUIRE(json_get_bool(reply, "ok", ok));
    CHECK(ok);

    REQUIRE(client.send_line(encode_subscribe("EURUSD")));

    const std::vector<Tick> ticks = recorded_ticks();
    std::size_t orders_seen = 0;
    std::size_t acks_sent   = 0;

    for (const Tick& t : ticks) {
        REQUIRE(client.send_line(encode(t)));
        std::string resp;
        REQUIRE(client.recv_line(resp));        // exactly one reply per tick
        const MsgKind k = kind_of(resp);
        if (k == MsgKind::Order) {
            ++orders_seen;
            Order o;
            REQUIRE(parse_order(resp, o));
            CHECK(o.symbol == "EURUSD");
            CHECK((o.side == Side::Buy || o.side == Side::Sell));
            // simulate MT5 OrderSend success and ack with the correlation id
            Ack a;
            a.id      = o.id;
            a.ok      = true;
            a.retcode = 10009;  // TRADE_RETCODE_DONE
            a.message = "done";
            REQUIRE(client.send_line(encode(a)));
            ++acks_sent;
        } else {
            CHECK(k == MsgKind::Nop);
        }
    }
    REQUIRE(client.send_line(encode_bye()));
    server.join();

    CHECK(server_stats.ticks == ticks.size());
    CHECK(server_stats.orders == orders_seen);
    CHECK(server_stats.acks == acks_sent);
    CHECK(server_stats.nops == ticks.size() - orders_seen);
    CHECK(orders_seen > 0);                     // the tape must actually trade
}

TEST_CASE("bridge disconnects a silent client after the idle timeout",
          "[mt5][itest][timeout]") {
    Listener            listener(0);
    const std::uint16_t port = listener.port();

    std::thread server([&] {
        LineSocket      conn = listener.accept();
        ExampleStrategy strat;
        (void)run_bridge(conn, strat, /*idle_timeout_ms=*/100, /*max_idle_intervals=*/3);
    });

    LineSocket client = LineSocket::connect("127.0.0.1", port);
    client.set_recv_timeout(4000);   // guard: fail fast rather than hang if broken
    REQUIRE(client.send_line(encode_hello("ITCHBridge.mq5", 1)));
    std::string reply;
    REQUIRE(client.recv_line(reply));
    CHECK(kind_of(reply) == MsgKind::Hello);

    // Go silent: the server should close the session on its idle timeout (~300ms).
    std::string line;
    const bool  got = client.recv_line(line);
    server.join();
    CHECK_FALSE(got);                 // the connection ended...
    CHECK_FALSE(client.timed_out());  // ...because the server closed it, not our guard
}

TEST_CASE("depth publish: an engine book streams to a client over TCP",
          "[mt5][itest][depth]") {
    Listener            listener(0);
    const std::uint16_t port = listener.port();

    std::thread server([&] {
        LineSocket conn = listener.accept();
        OrderBook  book;
        book.add(1, Side::Buy, 1000000, 100);
        book.add(2, Side::Buy, 999900, 200);
        book.add(3, Side::Sell, 1000100, 150);
        publish_depth(conn, "AAPL", book, 5);
    });

    LineSocket  client = LineSocket::connect("127.0.0.1", port);
    std::string line;
    REQUIRE(client.recv_line(line));
    CHECK(kind_of(line) == MsgKind::Depth);

    std::string                        sym;
    std::vector<std::pair<Price, Qty>> bids, asks;
    REQUIRE(parse_depth(line, sym, bids, asks));
    server.join();

    CHECK(sym == "AAPL");
    REQUIRE(bids.size() == 2);
    CHECK(bids[0].first == 1000000);
    CHECK(bids[0].second == 100u);
    CHECK(bids[1].first == 999900);
    CHECK(bids[1].second == 200u);
    REQUIRE(asks.size() == 1);
    CHECK(asks[0].first == 1000100);
    CHECK(asks[0].second == 150u);
}

TEST_CASE("signal publish: engine microstructure read streams to a client over TCP",
          "[mt5][itest][signal]") {
    Listener            listener(0);
    const std::uint16_t port = listener.port();

    std::thread server([&] {
        LineSocket conn = listener.accept();
        OrderBook  book;
        book.add(1, Side::Buy, 1000000, 100);   // bid $100.00 x100
        book.add(2, Side::Sell, 1000100, 50);   // ask $100.01 x50
        publish_signal(conn, "AAPL", book, 5);
    });

    LineSocket  client = LineSocket::connect("127.0.0.1", port);
    std::string line;
    REQUIRE(client.recv_line(line));
    CHECK(kind_of(line) == MsgKind::Signal);

    std::string sym;
    double      mid = 0, mp = 0, imb = 0, bps = 0;
    REQUIRE(parse_signal(line, sym, mid, mp, imb, bps));
    server.join();

    CHECK(sym == "AAPL");
    CHECK(mid > 100.0);
    CHECK(mid < 100.01);
    CHECK(imb > 0.0);   // more bid size than ask -> positive imbalance
}
