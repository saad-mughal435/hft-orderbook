#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include "book/book_set.hpp"
#include "book/order_book.hpp"
#include "itch/messages.hpp"

using namespace hftob;

TEST_CASE("add builds bid/ask levels and best quotes", "[book]") {
    OrderBook b;
    b.add(1, Side::Buy, 1000000, 100);   // $100.0000 x100
    b.add(2, Side::Buy, 999900, 200);    // $99.9900 x200
    b.add(3, Side::Sell, 1000100, 150);  // $100.0100 x150
    b.add(4, Side::Sell, 1000200, 50);

    Price px;
    Qty q;
    REQUIRE(b.best_bid(px, q));
    CHECK(px == 1000000);
    CHECK(q == 100u);
    REQUIRE(b.best_ask(px, q));
    CHECK(px == 1000100);
    CHECK(q == 150u);
    CHECK(b.order_count() == 4);
    CHECK(b.bid_levels() == 2);
    CHECK(b.ask_levels() == 2);
    CHECK(b.qty_at(Side::Buy, 999900) == 200u);
}

TEST_CASE("orders aggregate at a price level", "[book]") {
    OrderBook b;
    b.add(1, Side::Buy, 1000000, 100);
    b.add(2, Side::Buy, 1000000, 250);
    CHECK(b.qty_at(Side::Buy, 1000000) == 350u);
    CHECK(b.bid_levels() == 1);
    CHECK(b.order_count() == 2);
}

TEST_CASE("execute reduces shares; level erased at zero", "[book]") {
    OrderBook b;
    b.add(1, Side::Buy, 1000000, 100);
    CHECK(b.reduce(1, 40));
    CHECK(b.qty_at(Side::Buy, 1000000) == 60u);
    CHECK(b.order_count() == 1);
    CHECK(b.reduce(1, 60));
    CHECK(b.order_count() == 0);
    CHECK(b.bid_levels() == 0);
    Price px;
    Qty q;
    CHECK_FALSE(b.best_bid(px, q));
}

TEST_CASE("over-reduce clamps and removes the order", "[book]") {
    OrderBook b;
    b.add(1, Side::Sell, 1000100, 30);
    CHECK(b.reduce(1, 1000));
    CHECK(b.order_count() == 0);
    CHECK(b.ask_levels() == 0);
}

TEST_CASE("delete removes the order and its level contribution", "[book]") {
    OrderBook b;
    b.add(1, Side::Buy, 1000000, 100);
    b.add(2, Side::Buy, 1000000, 100);
    CHECK(b.remove(1));
    CHECK(b.qty_at(Side::Buy, 1000000) == 100u);
    CHECK(b.order_count() == 1);
    CHECK(b.bid_levels() == 1);
}

TEST_CASE("replace deletes original and adds new, inheriting side", "[book]") {
    OrderBook b;
    b.add(10, Side::Sell, 1000100, 100);
    CHECK(b.replace(10, 11, 60, 1000050));
    Price px;
    Qty q;
    REQUIRE(b.best_ask(px, q));
    CHECK(px == 1000050);
    CHECK(q == 60u);
    CHECK(b.qty_at(Side::Sell, 1000100) == 0u);
    CHECK(b.order_count() == 1);
}

TEST_CASE("operations on unknown refs are ignored", "[book]") {
    OrderBook b;
    CHECK_FALSE(b.reduce(99, 10));
    CHECK_FALSE(b.remove(99));
    CHECK_FALSE(b.replace(99, 100, 10, 1000000));
    CHECK(b.order_count() == 0);
}

TEST_CASE("apply() routes ITCH messages to book operations", "[book]") {
    OrderBook b;

    itch::Message add{};
    add.type = itch::MsgType::AddOrder;
    add.order_ref = 1;
    add.side = Side::Buy;
    add.price = 1000000;
    add.shares = 500;
    b.apply(add);
    CHECK(b.qty_at(Side::Buy, 1000000) == 500u);

    itch::Message exec{};
    exec.type = itch::MsgType::OrderExecuted;
    exec.order_ref = 1;
    exec.shares = 200;
    b.apply(exec);
    CHECK(b.qty_at(Side::Buy, 1000000) == 300u);

    itch::Message trade{};
    trade.type = itch::MsgType::Trade;
    trade.shares = 100;
    b.apply(trade);  // tape-only, no book change
    CHECK(b.qty_at(Side::Buy, 1000000) == 300u);

    itch::Message del{};
    del.type = itch::MsgType::OrderDelete;
    del.order_ref = 1;
    b.apply(del);
    CHECK(b.order_count() == 0);
}

TEST_CASE("depth returns the top-n levels, best first, clamped to the side", "[book]") {
    OrderBook b;
    b.add(1, Side::Buy, 1000000, 100);
    b.add(2, Side::Buy, 999900, 200);
    b.add(3, Side::Buy, 999800, 300);
    b.add(4, Side::Sell, 1000100, 150);
    b.add(5, Side::Sell, 1000200, 250);

    const auto bids = b.depth(Side::Buy, 2);
    REQUIRE(bids.size() == 2);
    CHECK(bids[0].first == 1000000);  CHECK(bids[0].second == 100u);  // best bid first
    CHECK(bids[1].first == 999900);   CHECK(bids[1].second == 200u);

    const auto asks = b.asks(10);     // ask for more than exist -> clamped
    REQUIRE(asks.size() == 2);
    CHECK(asks[0].first == 1000100);  // best ask first (ascending)
    CHECK(asks[1].first == 1000200);
}

TEST_CASE("BookSet routes by stock_locate and names books from StockDirectory",
          "[book][bookset]") {
    BookSet bs;

    itch::Message r1{};
    r1.type = itch::MsgType::StockDirectory;
    r1.stock_locate = 1;
    std::memcpy(r1.stock, "AAPL    ", 8);
    bs.apply(r1);
    itch::Message r2{};
    r2.type = itch::MsgType::StockDirectory;
    r2.stock_locate = 2;
    std::memcpy(r2.stock, "MSFT    ", 8);
    bs.apply(r2);

    auto add = [&](std::uint16_t loc, std::uint64_t ref, Side s, Price p, Qty q) {
        itch::Message m{};
        m.type = itch::MsgType::AddOrder;
        m.stock_locate = loc;
        m.order_ref = ref;
        m.side = s;
        m.price = p;
        m.shares = q;
        bs.apply(m);
    };
    add(1, 10, Side::Buy, 1500000, 100);   // AAPL
    add(1, 11, Side::Sell, 1500100, 50);   // AAPL
    add(2, 20, Side::Buy, 3000000, 200);   // MSFT

    CHECK(bs.book_count() == 2);
    CHECK(bs.symbol(1) == "AAPL");          // trailing spaces trimmed
    CHECK(bs.symbol(2) == "MSFT");
    CHECK(bs.total_orders() == 3);

    const OrderBook* aapl = bs.book("AAPL");
    REQUIRE(aapl != nullptr);
    CHECK(aapl->order_count() == 2);
    Price px = 0;
    Qty   qy = 0;
    REQUIRE(aapl->best_bid(px, qy));
    CHECK(px == 1500000);
    CHECK(qy == 100u);

    const OrderBook* msft = bs.book(2);
    REQUIRE(msft != nullptr);
    CHECK(msft->order_count() == 1);

    // An execute on AAPL's order routes to AAPL only; MSFT is untouched.
    itch::Message e{};
    e.type = itch::MsgType::OrderExecuted;
    e.stock_locate = 1;
    e.order_ref = 10;
    e.shares = 40;
    bs.apply(e);
    CHECK(aapl->qty_at(Side::Buy, 1500000) == 60u);
    CHECK(msft->order_count() == 1);
}
