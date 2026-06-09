#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

#include "fix/fields.hpp"
#include "fix/message.hpp"

using namespace hftob::fix;

TEST_CASE("NewOrderSingle encodes, validates and round-trips", "[fix]") {
    const std::string s = new_order_single("HFTOB", "EXCH", 1, "CLORD-1", "AAPL",
                                           Side::Buy, 100, OrdType::Limit, 150.25);
    CHECK(valid_checksum(s));

    Message m;
    REQUIRE(parse(s, m));
    CHECK(m.get_or(tag::BeginString) == "FIX.4.4");
    CHECK(m.get_or(tag::MsgType) == "D");
    CHECK(m.get_or(tag::ClOrdID) == "CLORD-1");
    CHECK(m.get_or(tag::Symbol) == "AAPL");
    CHECK(m.get_or(tag::Side) == "1");          // Buy
    CHECK(m.get_or(tag::OrderQty) == "100");
    CHECK(m.get_or(tag::OrdType) == "2");        // Limit
    CHECK(m.get_or(tag::Price) == "150.2500");
}

TEST_CASE("BodyLength equals the actual body byte count", "[fix]") {
    const std::string s = new_order_single("S", "T", 7, "C", "MSFT", Side::Sell, 50, OrdType::Market);

    const std::string soh1(1, SOH);
    const std::size_t e9  = s.find(SOH, s.find("9="));  // SOH ending the BodyLength field
    const std::size_t p10 = s.rfind(soh1 + "10=");      // SOH preceding the CheckSum field
    REQUIRE(e9 != std::string::npos);
    REQUIRE(p10 != std::string::npos);

    Message m;
    REQUIRE(parse(s, m));
    CHECK(static_cast<long>(p10 - e9) == std::atol(m.get_or(tag::BodyLength).c_str()));
    CHECK(m.get(tag::Price) == nullptr);  // a market order carries no price
}

TEST_CASE("checksum detects a tampered message", "[fix]") {
    std::string s = new_order_single("S", "T", 1, "C", "AAPL", Side::Buy, 10, OrdType::Limit, 99.99);
    CHECK(valid_checksum(s));

    const std::size_t pos = s.find("AAPL");
    REQUIRE(pos != std::string::npos);
    s[pos] = 'B';                       // flip a byte in the symbol value
    CHECK_FALSE(valid_checksum(s));
}

TEST_CASE("ExecutionReport (fill) round-trips", "[fix]") {
    const std::string s = execution_report("EXCH", "HFTOB", 2, "OID-9", "CLORD-1", "AAPL",
                                           Side::Buy, ExecType::Fill, OrdStatus::Filled, 100, 0, 150.25);
    CHECK(valid_checksum(s));

    Message m;
    REQUIRE(parse(s, m));
    CHECK(m.get_or(tag::MsgType) == "8");
    CHECK(m.get_or(tag::ExecType) == "2");       // Fill
    CHECK(m.get_or(tag::OrdStatus) == "2");      // Filled
    CHECK(m.get_or(tag::OrderID) == "OID-9");
    CHECK(m.get_or(tag::CumQty) == "100");
    CHECK(m.get_or(tag::LeavesQty) == "0");
}
