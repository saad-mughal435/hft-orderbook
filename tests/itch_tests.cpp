#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include "itch/decoder.hpp"

using namespace hftob;
using namespace hftob::itch;

namespace {

using Buf = std::vector<std::uint8_t>;

void put16(Buf& b, std::size_t o, std::uint16_t v) {
    b[o] = std::uint8_t(v >> 8);
    b[o + 1] = std::uint8_t(v);
}
void put32(Buf& b, std::size_t o, std::uint32_t v) {
    b[o] = std::uint8_t(v >> 24);
    b[o + 1] = std::uint8_t(v >> 16);
    b[o + 2] = std::uint8_t(v >> 8);
    b[o + 3] = std::uint8_t(v);
}
void put64(Buf& b, std::size_t o, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[o + i] = std::uint8_t(v >> (8 * (7 - i)));
}
void put48(Buf& b, std::size_t o, std::uint64_t v) {
    for (int i = 0; i < 6; ++i) b[o + i] = std::uint8_t(v >> (8 * (5 - i)));
}
void put_stock(Buf& b, std::size_t o, const char* s) {
    std::memset(&b[o], ' ', 8);
    std::memcpy(&b[o], s, std::strlen(s));
}

Buf header(char type, std::size_t len, std::uint16_t locate, std::uint64_t ts) {
    Buf b(len, 0);
    b[0] = std::uint8_t(type);
    put16(b, 1, locate);
    put16(b, 3, 0);  // tracking number
    put48(b, 5, ts);
    return b;
}

}  // namespace

TEST_CASE("message_length table matches the spec", "[itch]") {
    CHECK(message_length('A') == 36);
    CHECK(message_length('F') == 40);
    CHECK(message_length('E') == 31);
    CHECK(message_length('C') == 36);
    CHECK(message_length('X') == 23);
    CHECK(message_length('D') == 19);
    CHECK(message_length('U') == 35);
    CHECK(message_length('P') == 44);
    CHECK(message_length('Q') == 40);
    CHECK(message_length('S') == 12);
    CHECK(message_length('Z') == 0);
}

TEST_CASE("decode Add Order (A)", "[itch]") {
    Buf b = header('A', 36, 7, 123456789ULL);
    put64(b, 11, 42);
    b[19] = 'B';
    put32(b, 20, 100);
    put_stock(b, 24, "AAPL");
    put32(b, 32, 1505000);  // $150.5000

    Message m;
    REQUIRE(decode(b.data(), b.size(), m));
    CHECK(m.type == MsgType::AddOrder);
    CHECK(m.stock_locate == 7);
    CHECK(m.timestamp == 123456789ULL);
    CHECK(m.order_ref == 42);
    CHECK(m.side == Side::Buy);
    CHECK(m.shares == 100);
    CHECK(m.price == 1505000);
    CHECK(std::memcmp(m.stock, "AAPL    ", 8) == 0);
}

TEST_CASE("decode Order Executed (E)", "[itch]") {
    Buf b = header('E', 31, 1, 5);
    put64(b, 11, 42);
    put32(b, 19, 30);
    put64(b, 23, 999);

    Message m;
    REQUIRE(decode(b.data(), b.size(), m));
    CHECK(m.type == MsgType::OrderExecuted);
    CHECK(m.order_ref == 42);
    CHECK(m.shares == 30);
    CHECK(m.match_number == 999);
}

TEST_CASE("decode Order Cancel (X) and Delete (D)", "[itch]") {
    Buf x = header('X', 23, 1, 5);
    put64(x, 11, 42);
    put32(x, 19, 25);
    Message mx;
    REQUIRE(decode(x.data(), x.size(), mx));
    CHECK(mx.type == MsgType::OrderCancel);
    CHECK(mx.order_ref == 42);
    CHECK(mx.shares == 25);

    Buf d = header('D', 19, 1, 5);
    put64(d, 11, 42);
    Message md;
    REQUIRE(decode(d.data(), d.size(), md));
    CHECK(md.type == MsgType::OrderDelete);
    CHECK(md.order_ref == 42);
}

TEST_CASE("decode Order Replace (U)", "[itch]") {
    Buf b = header('U', 35, 1, 5);
    put64(b, 11, 42);       // original
    put64(b, 19, 43);       // new
    put32(b, 27, 80);
    put32(b, 31, 1500000);

    Message m;
    REQUIRE(decode(b.data(), b.size(), m));
    CHECK(m.type == MsgType::OrderReplace);
    CHECK(m.order_ref == 42);
    CHECK(m.new_order_ref == 43);
    CHECK(m.shares == 80);
    CHECK(m.price == 1500000);
}

TEST_CASE("decode rejects short buffers and unknown types", "[itch]") {
    Buf a = header('A', 36, 1, 1);
    Message m;
    CHECK_FALSE(decode(a.data(), 20, m));  // too short for an Add body
    CHECK_FALSE(decode(a.data(), 5, m));   // shorter than the common header
    Buf z = header('Z', 36, 1, 1);
    CHECK_FALSE(decode(z.data(), z.size(), m));  // unknown type
    CHECK_FALSE(decode(nullptr, 36, m));
}
