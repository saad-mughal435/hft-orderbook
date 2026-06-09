#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "core/types.hpp"

namespace hftob {
namespace mt5 {

// Versioned newline-delimited-JSON (NDJSON) bridge protocol between the MetaTrader
// 5 Expert Advisor (client) and the `mt5d` engine (server). One compact JSON
// object per line. The schema is intentionally flat (no nesting, plain-ASCII
// string values) so it round-trips through this small dependency-free codec and
// through MQL5's limited string handling on the EA side.
//
//   ticks   : EA  -> engine   {"t":"tick","v":1,"symbol":"EURUSD","time":..,"bid":..,"ask":..,"last":..,"volume":..}
//   orders  : engine -> EA     {"t":"order","v":1,"id":7,"symbol":"EURUSD","side":"B","volume":0.1,"price":0,"kind":"market"}
//   acks    : EA  -> engine    {"t":"ack","v":1,"id":7,"ok":true,"retcode":10009,"message":"done"}
// plus hello / subscribe / nop / bye / heartbeat for session control. Every order
// carries a correlation `id` echoed back on its ack.

constexpr int kProtocolVersion = 1;

enum class MsgKind { Unknown, Hello, Subscribe, Tick, Order, Ack, Nop, Bye, Heartbeat, Depth, Signal };

struct Tick {
    std::string   symbol;
    std::uint64_t time   = 0;   // epoch (seconds or ms — opaque to the engine)
    double        bid    = 0.0;
    double        ask    = 0.0;
    double        last   = 0.0;
    std::uint64_t volume = 0;
};

struct Order {
    std::uint64_t id     = 0;          // correlation id (echoed on the ack)
    std::string   symbol;
    Side          side   = Side::None; // Buy / Sell
    double        volume = 0.0;        // lots
    double        price  = 0.0;        // 0 => market order
    std::string   kind   = "market";   // "market" | "limit"
};

struct Ack {
    std::uint64_t id      = 0;     // correlation id of the order
    bool          ok      = false;
    int           retcode = 0;     // MT5 MqlTradeResult.retcode (10009 == DONE)
    std::string   message;
};

// ---- flat-JSON field extraction -------------------------------------------
// Single-pass helpers for the controlled schema above. Not a general JSON parser
// (values are unnested apart from the depth ladders), but it is robust where it
// matters: a key is only matched at a real object-key position (preceded by '{'
// or ',' and followed by ':'), so a key name appearing inside a string *value*
// is never mistaken for the field, and string reads honour `\"` / `\\` escapes.

namespace detail {

inline std::size_t value_pos(const std::string& s, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    std::size_t from = 0;
    for (;;) {
        const std::size_t k = s.find(pat, from);
        if (k == std::string::npos) return std::string::npos;
        // The match must be a key: the previous non-space char is '{' or ','.
        std::size_t p = k;
        while (p > 0 && (s[p - 1] == ' ' || s[p - 1] == '\t')) --p;
        const bool at_key = (p == 0) || (s[p - 1] == '{') || (s[p - 1] == ',');
        // ...and it must be followed by ':'.
        std::size_t c = k + pat.size();
        while (c < s.size() && (s[c] == ' ' || s[c] == '\t')) ++c;
        if (at_key && c < s.size() && s[c] == ':') {
            std::size_t i = c + 1;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            return i;
        }
        from = k + 1;  // false positive (e.g. inside a value); keep scanning
    }
}

inline std::string fmt_double(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.10g", v);
    return std::string(b);
}

inline std::string esc(const std::string& s) {  // escape for a JSON string value
    std::string o;
    o.reserve(s.size() + 2);
    for (const char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
        else if (c == '\n')        { o += "\\n"; }
        else if (c == '\t')        { o += "\\t"; }
        else                       { o.push_back(c); }
    }
    return o;
}

}  // namespace detail

inline bool json_get_str(const std::string& s, const std::string& key, std::string& out) {
    std::size_t i = detail::value_pos(s, key);
    if (i == std::string::npos || i >= s.size() || s[i] != '"') return false;
    ++i;
    std::string r;
    while (i < s.size()) {
        const char ch = s[i];
        if (ch == '\\' && i + 1 < s.size()) {       // unescape \" \\ \/ \n \t
            const char nx = s[i + 1];
            if (nx == '"' || nx == '\\' || nx == '/') { r.push_back(nx); i += 2; continue; }
            if (nx == 'n') { r.push_back('\n'); i += 2; continue; }
            if (nx == 't') { r.push_back('\t'); i += 2; continue; }
            r.push_back(ch); ++i; continue;          // unknown escape: keep literally
        }
        if (ch == '"') { out.swap(r); return true; }
        r.push_back(ch);
        ++i;
    }
    return false;  // unterminated string
}
inline bool json_get_int(const std::string& s, const std::string& key, long long& out) {
    std::size_t i = detail::value_pos(s, key);
    if (i == std::string::npos) return false;
    const char* p   = s.c_str() + i;
    char*       end = nullptr;
    const long long v = std::strtoll(p, &end, 10);
    if (end == p) return false;
    out = v;
    return true;
}
inline bool json_get_double(const std::string& s, const std::string& key, double& out) {
    std::size_t i = detail::value_pos(s, key);
    if (i == std::string::npos) return false;
    const char* p   = s.c_str() + i;
    char*       end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == p) return false;
    out = v;
    return true;
}
inline bool json_get_bool(const std::string& s, const std::string& key, bool& out) {
    std::size_t i = detail::value_pos(s, key);
    if (i == std::string::npos) return false;
    if (s.compare(i, 4, "true") == 0)  { out = true;  return true; }
    if (s.compare(i, 5, "false") == 0) { out = false; return true; }
    return false;
}

inline MsgKind kind_of(const std::string& line) {
    std::string t;
    if (!json_get_str(line, "t", t)) return MsgKind::Unknown;
    if (t == "hello")     return MsgKind::Hello;
    if (t == "subscribe") return MsgKind::Subscribe;
    if (t == "tick")      return MsgKind::Tick;
    if (t == "order")     return MsgKind::Order;
    if (t == "ack")       return MsgKind::Ack;
    if (t == "nop")       return MsgKind::Nop;
    if (t == "bye")       return MsgKind::Bye;
    if (t == "heartbeat") return MsgKind::Heartbeat;
    if (t == "depth")     return MsgKind::Depth;
    if (t == "signal")    return MsgKind::Signal;
    return MsgKind::Unknown;
}

// ---- encoders (each returns one NDJSON line, terminated with '\n') ----------

inline std::string encode_hello(const std::string& client, std::uint64_t account) {
    return "{\"t\":\"hello\",\"v\":" + std::to_string(kProtocolVersion) +
           ",\"client\":\"" + detail::esc(client) + "\",\"account\":" +
           std::to_string(account) + "}\n";
}
inline std::string encode_hello_reply(const std::string& server, bool ok) {
    return "{\"t\":\"hello\",\"v\":" + std::to_string(kProtocolVersion) +
           ",\"server\":\"" + detail::esc(server) + "\",\"ok\":" + (ok ? "true" : "false") + "}\n";
}
inline std::string encode_subscribe(const std::string& symbol) {
    return "{\"t\":\"subscribe\",\"v\":" + std::to_string(kProtocolVersion) +
           ",\"symbol\":\"" + detail::esc(symbol) + "\"}\n";
}
inline std::string encode(const Tick& t) {
    return "{\"t\":\"tick\",\"v\":" + std::to_string(kProtocolVersion) +
           ",\"symbol\":\"" + detail::esc(t.symbol) + "\"" +
           ",\"time\":" + std::to_string(t.time) +
           ",\"bid\":" + detail::fmt_double(t.bid) +
           ",\"ask\":" + detail::fmt_double(t.ask) +
           ",\"last\":" + detail::fmt_double(t.last) +
           ",\"volume\":" + std::to_string(t.volume) + "}\n";
}
inline std::string encode(const Order& o) {
    const char side = (o.side == Side::Buy) ? 'B' : 'S';
    return "{\"t\":\"order\",\"v\":" + std::to_string(kProtocolVersion) +
           ",\"id\":" + std::to_string(o.id) +
           ",\"symbol\":\"" + detail::esc(o.symbol) + "\"" +
           ",\"side\":\"" + std::string(1, side) + "\"" +
           ",\"volume\":" + detail::fmt_double(o.volume) +
           ",\"price\":" + detail::fmt_double(o.price) +
           ",\"kind\":\"" + detail::esc(o.kind) + "\"}\n";
}
inline std::string encode(const Ack& a) {
    return "{\"t\":\"ack\",\"v\":" + std::to_string(kProtocolVersion) +
           ",\"id\":" + std::to_string(a.id) +
           ",\"ok\":" + (a.ok ? "true" : "false") +
           ",\"retcode\":" + std::to_string(a.retcode) +
           ",\"message\":\"" + detail::esc(a.message) + "\"}\n";
}
inline std::string encode_nop()       { return "{\"t\":\"nop\",\"v\":1}\n"; }
inline std::string encode_bye()       { return "{\"t\":\"bye\",\"v\":1}\n"; }
inline std::string encode_heartbeat(std::uint64_t time) {
    return "{\"t\":\"heartbeat\",\"v\":1,\"time\":" + std::to_string(time) + "}\n";
}

/// A depth snapshot published by the engine: top-N reconstructed book levels for
/// a symbol, each a `[price_ticks, qty]` pair (bids best-first, asks best-first).
/// This is how the ITCH-reconstructed book streams out to MetaTrader / clients.
inline std::string encode_depth(const std::string& symbol,
                                const std::vector<std::pair<Price, Qty>>& bids,
                                const std::vector<std::pair<Price, Qty>>& asks) {
    std::string s = "{\"t\":\"depth\",\"v\":" + std::to_string(kProtocolVersion) +
                    ",\"symbol\":\"" + detail::esc(symbol) + "\",\"bids\":[";
    for (std::size_t i = 0; i < bids.size(); ++i) {
        if (i) s.push_back(',');
        s += "[" + std::to_string(bids[i].first) + "," + std::to_string(bids[i].second) + "]";
    }
    s += "],\"asks\":[";
    for (std::size_t i = 0; i < asks.size(); ++i) {
        if (i) s.push_back(',');
        s += "[" + std::to_string(asks[i].first) + "," + std::to_string(asks[i].second) + "]";
    }
    s += "]}\n";
    return s;
}

// ---- parsers (return false on a missing required field) ---------------------

inline bool parse_tick(const std::string& line, Tick& out) {
    if (!json_get_str(line, "symbol", out.symbol)) return false;
    if (!json_get_double(line, "bid", out.bid)) return false;
    if (!json_get_double(line, "ask", out.ask)) return false;
    json_get_double(line, "last", out.last);           // optional
    long long t = 0, v = 0;
    if (json_get_int(line, "time", t)) out.time = static_cast<std::uint64_t>(t);
    if (json_get_int(line, "volume", v)) out.volume = static_cast<std::uint64_t>(v);
    return true;
}
inline bool parse_order(const std::string& line, Order& out) {
    long long id = 0;
    if (!json_get_int(line, "id", id)) return false;
    out.id = static_cast<std::uint64_t>(id);
    if (!json_get_str(line, "symbol", out.symbol)) return false;
    std::string side;
    if (!json_get_str(line, "side", side) || side.empty()) return false;
    out.side = (side[0] == 'B') ? Side::Buy : Side::Sell;
    json_get_double(line, "volume", out.volume);
    json_get_double(line, "price", out.price);
    json_get_str(line, "kind", out.kind);
    return true;
}
inline bool parse_ack(const std::string& line, Ack& out) {
    long long id = 0, rc = 0;
    if (!json_get_int(line, "id", id)) return false;
    out.id = static_cast<std::uint64_t>(id);
    json_get_bool(line, "ok", out.ok);
    if (json_get_int(line, "retcode", rc)) out.retcode = static_cast<int>(rc);
    json_get_str(line, "message", out.message);
    return true;
}

// Parse a "[[price,qty],[price,qty],...]" ladder following `key` into `out`.
inline bool parse_levels(const std::string& line, const std::string& key,
                         std::vector<std::pair<Price, Qty>>& out) {
    std::size_t i = detail::value_pos(line, key);
    if (i == std::string::npos || i >= line.size() || line[i] != '[') return false;
    out.clear();
    ++i;  // step past the outer '['
    while (i < line.size() && line[i] != ']') {
        if (line[i] != '[') { ++i; continue; }       // skip separators
        ++i;                                          // into the pair
        char*           end = nullptr;
        const long long p   = std::strtoll(line.c_str() + i, &end, 10);
        if (end == line.c_str() + i) return false;
        i = static_cast<std::size_t>(end - line.c_str());
        while (i < line.size() && (line[i] == ',' || line[i] == ' ')) ++i;
        char*           end2 = nullptr;
        const long long q    = std::strtoll(line.c_str() + i, &end2, 10);
        if (end2 == line.c_str() + i) return false;
        i = static_cast<std::size_t>(end2 - line.c_str());
        while (i < line.size() && line[i] != ']') ++i;  // to the pair's ']'
        if (i < line.size()) ++i;                       // past it
        out.emplace_back(static_cast<Price>(p), static_cast<Qty>(q));
    }
    return true;
}

inline bool parse_depth(const std::string& line, std::string& symbol,
                        std::vector<std::pair<Price, Qty>>& bids,
                        std::vector<std::pair<Price, Qty>>& asks) {
    if (!json_get_str(line, "symbol", symbol)) return false;
    parse_levels(line, "bids", bids);
    parse_levels(line, "asks", asks);
    return true;
}

/// A derived-signal message: the engine's microstructure read for a symbol (mid,
/// micro-price, top-of-book imbalance, spread in bps) streamed out to subscribers.
inline std::string encode_signal(const std::string& symbol, double mid, double microprice,
                                 double imbalance, double spread_bps) {
    return "{\"t\":\"signal\",\"v\":" + std::to_string(kProtocolVersion) +
           ",\"symbol\":\"" + detail::esc(symbol) + "\"" +
           ",\"mid\":" + detail::fmt_double(mid) +
           ",\"microprice\":" + detail::fmt_double(microprice) +
           ",\"imbalance\":" + detail::fmt_double(imbalance) +
           ",\"spread_bps\":" + detail::fmt_double(spread_bps) + "}\n";
}
inline bool parse_signal(const std::string& line, std::string& symbol, double& mid,
                         double& microprice, double& imbalance, double& spread_bps) {
    if (!json_get_str(line, "symbol", symbol)) return false;
    json_get_double(line, "mid", mid);
    json_get_double(line, "microprice", microprice);
    json_get_double(line, "imbalance", imbalance);
    json_get_double(line, "spread_bps", spread_bps);
    return true;
}

}  // namespace mt5
}  // namespace hftob
