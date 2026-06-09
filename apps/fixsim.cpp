// fixsim - emit and parse a couple of FIX 4.4 order-entry messages, printing them
// with SOH shown as '|'. A tiny demo of the order-entry codec that complements the
// ITCH market-data side (market data in, orders out).

#include <iostream>
#include <string>

#include "fix/fields.hpp"
#include "fix/message.hpp"

using namespace hftob::fix;

namespace {
std::string printable(std::string s) {
    for (char& c : s)
        if (c == SOH) c = '|';
    return s;
}
}  // namespace

int main() {
    const std::string ord = new_order_single("HFTOB", "EXCH", 1, "CLORD-1", "AAPL",
                                             Side::Buy, 100, OrdType::Limit, 150.25);
    std::cout << "NewOrderSingle (35=D):\n  " << printable(ord) << "\n"
              << "  checksum valid: " << (valid_checksum(ord) ? "yes" : "no") << "\n";

    Message m;
    parse(ord, m);
    std::cout << "  parsed " << m.fields.size() << " fields; symbol=" << m.get_or(tag::Symbol)
              << " side=" << m.get_or(tag::Side) << " qty=" << m.get_or(tag::OrderQty)
              << " px=" << m.get_or(tag::Price) << "\n\n";

    const std::string er = execution_report("EXCH", "HFTOB", 1, "OID-1", "CLORD-1", "AAPL",
                                            Side::Buy, ExecType::Fill, OrdStatus::Filled, 100, 0, 150.25);
    std::cout << "ExecutionReport (35=8):\n  " << printable(er) << "\n"
              << "  checksum valid: " << (valid_checksum(er) ? "yes" : "no") << "\n";
    return 0;
}
