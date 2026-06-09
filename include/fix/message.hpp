#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "fix/fields.hpp"

namespace hftob {
namespace fix {

/// An ordered list of FIX `tag=value` fields. Order matters in FIX, so a vector
/// (not a map) preserves it; `get` is a small linear scan (FIX messages are short).
struct Message {
    std::vector<std::pair<int, std::string>> fields;

    void set(int t, const std::string& v) { fields.emplace_back(t, v); }
    void set(int t, long long v) { fields.emplace_back(t, std::to_string(v)); }
    void set(int t, char c) { fields.emplace_back(t, std::string(1, c)); }

    const std::string* get(int t) const {
        for (const auto& f : fields)
            if (f.first == t) return &f.second;
        return nullptr;
    }
    std::string get_or(int t, const std::string& dflt = std::string()) const {
        const std::string* p = get(t);
        return p ? *p : dflt;
    }
};

/// Sum of all bytes mod 256 - the FIX CheckSum (tag 10), formatted as 3 digits.
inline unsigned checksum(const std::string& s) {
    unsigned sum = 0;
    for (const unsigned char c : s) sum += c;
    return sum % 256;
}

inline std::string price_str(double px) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.4f", px);
    return std::string(b);
}

/// Encode a full FIX message: `8=begin | 9=bodylen | 35=type | <body> | 10=cksum`,
/// computing BodyLength (bytes of the body) and CheckSum automatically.
inline std::string encode(const std::string& begin_string, const std::string& msg_type,
                          const Message& body) {
    std::string b = "35=" + msg_type + SOH;
    for (const auto& f : body.fields)
        b += std::to_string(f.first) + "=" + f.second + SOH;

    const std::string head = "8=" + begin_string + SOH + "9=" + std::to_string(b.size()) + SOH;
    std::string       m    = head + b;

    char cs[8];
    std::snprintf(cs, sizeof(cs), "%03u", checksum(m));
    m += "10=" + std::string(cs) + SOH;
    return m;
}

/// Parse every `tag=value` SOH-delimited field (including 8/9/35/10) into `out`.
inline bool parse(const std::string& s, Message& out) {
    out.fields.clear();
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t eq = s.find('=', i);
        if (eq == std::string::npos) break;
        const std::size_t soh = s.find(SOH, eq + 1);
        if (soh == std::string::npos) break;
        const int t = std::atoi(s.substr(i, eq - i).c_str());
        out.fields.emplace_back(t, s.substr(eq + 1, soh - eq - 1));
        i = soh + 1;
    }
    return !out.fields.empty();
}

/// Validate the trailing `10=CheckSum` against the bytes that precede it (the
/// checksum covers everything up to and including the SOH before `10=`).
inline bool valid_checksum(const std::string& s) {
    const std::string pat = std::string(1, SOH) + "10=";
    const std::size_t p   = s.rfind(pat);
    if (p == std::string::npos) return false;
    const unsigned    want = checksum(s.substr(0, p + 1));
    const std::string got  = s.substr(p + 1 + 3, 3);  // 3 digits after "10="
    return std::atoi(got.c_str()) == static_cast<int>(want);
}

// ---- order-entry message builders -----------------------------------------

inline std::string new_order_single(const std::string& sender, const std::string& target,
                                    long long seq, const std::string& clordid,
                                    const std::string& symbol, Side side, long long qty,
                                    OrdType type, double price = 0.0) {
    Message m;
    m.set(tag::SenderCompID, sender);
    m.set(tag::TargetCompID, target);
    m.set(tag::MsgSeqNum, seq);
    m.set(tag::ClOrdID, clordid);
    m.set(tag::Symbol, symbol);
    m.set(tag::Side, static_cast<char>(side));
    m.set(tag::OrderQty, qty);
    m.set(tag::OrdType, static_cast<char>(type));
    if (type == OrdType::Limit) m.set(tag::Price, price_str(price));
    return encode("FIX.4.4", "D", m);
}

inline std::string execution_report(const std::string& sender, const std::string& target,
                                    long long seq, const std::string& orderid,
                                    const std::string& clordid, const std::string& symbol,
                                    Side side, ExecType et, OrdStatus os, long long cumqty,
                                    long long leavesqty, double avgpx) {
    Message m;
    m.set(tag::SenderCompID, sender);
    m.set(tag::TargetCompID, target);
    m.set(tag::MsgSeqNum, seq);
    m.set(tag::OrderID, orderid);
    m.set(tag::ClOrdID, clordid);
    m.set(tag::Symbol, symbol);
    m.set(tag::Side, static_cast<char>(side));
    m.set(tag::ExecType, static_cast<char>(et));
    m.set(tag::OrdStatus, static_cast<char>(os));
    m.set(tag::CumQty, cumqty);
    m.set(tag::LeavesQty, leavesqty);
    m.set(tag::AvgPx, price_str(avgpx));
    return encode("FIX.4.4", "8", m);
}

}  // namespace fix
}  // namespace hftob
