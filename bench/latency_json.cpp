// latency_json - per-ITCH-message-type decode-to-book latency histogram, emitted
// as a stable JSON document for the site to render. Closes issue #1.
//
//   latency_json [N] [out.json]
//
// Replays a deterministic synthetic ITCH 5.0 session (fixed seed, so reruns are
// directly comparable) and, for every message, times ONLY decode + book.apply
// with steady_clock - no file IO, no allocation, no setup in the timed region.
// Latencies are bucketed by ITCH message type into per-type LatencyHist instances
// plus an overall histogram, then p50/p95/p99/max are printed as JSON.
//
// These are relative figures from a virtualized CI runner (see README): fair for
// an A/B and for catching regressions across runs on the same hardware, NOT
// absolute trading-hardware numbers.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <ostream>
#include <vector>

#include "book/order_book.hpp"
#include "core/latency_hist.hpp"
#include "itch/decoder.hpp"
#include "itch/messages.hpp"
#include "feed/synthetic.hpp"

using namespace hftob;

namespace {

// The ITCH message types this engine reconstructs a book from, in report order.
// Add/Execute/Cancel/Delete/Replace are the required breakdown; Add-with-MPID and
// Execute-with-Price are folded into their family so the buckets stay meaningful.
struct TypeInfo {
    itch::MsgType type;
    const char*   name;
    char          code;
};
constexpr TypeInfo kTypes[] = {
    {itch::MsgType::AddOrder,               "Add Order",                'A'},
    {itch::MsgType::AddOrderMpid,           "Add Order (MPID)",         'F'},
    {itch::MsgType::OrderExecuted,          "Order Executed",           'E'},
    {itch::MsgType::OrderExecutedWithPrice, "Order Executed w/ Price",  'C'},
    {itch::MsgType::OrderCancel,            "Order Cancel",             'X'},
    {itch::MsgType::OrderDelete,            "Order Delete",             'D'},
    {itch::MsgType::OrderReplace,           "Order Replace",            'U'},
};
constexpr std::size_t kNTypes = sizeof(kTypes) / sizeof(kTypes[0]);

int index_of(itch::MsgType t) {
    for (std::size_t i = 0; i < kNTypes; ++i)
        if (kTypes[i].type == t) return static_cast<int>(i);
    return -1;  // tape-only / reference data (P/Q/R/S): decoded but no book apply
}

void emit_hist_fields(std::ostream& os, const LatencyHist& h) {
    os << "\"count\": " << h.count()
       << ", \"p50_ns\": " << h.percentile(50)
       << ", \"p95_ns\": " << h.percentile(95)
       << ", \"p99_ns\": " << h.percentile(99)
       << ", \"max_ns\": " << h.max();
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t n_messages = 200000;     // default sample size
    const char* out_path   = nullptr;
    if (argc >= 2) {
        const long n = std::strtol(argv[1], nullptr, 10);
        if (n <= 0) { std::cerr << "error: N must be positive\n"; return 2; }
        n_messages = static_cast<std::size_t>(n);
    }
    if (argc >= 3) out_path = argv[2];

    // --- build the synthetic session OUTSIDE the timed region (fixed seed) ----
    constexpr std::uint32_t kSeed = 1;
    const std::vector<std::uint8_t> data = make_synthetic_itch(n_messages, kSeed);

    // Pre-split into framed message spans so the timed region does no framing
    // arithmetic beyond reading the message body - decode + apply only.
    struct Span { const std::uint8_t* p; std::size_t len; };
    std::vector<Span> spans;
    spans.reserve(n_messages);
    for (std::size_t off = 0; off + 2 <= data.size();) {
        const std::size_t mlen = (static_cast<std::size_t>(data[off]) << 8) |
                                 static_cast<std::size_t>(data[off + 1]);
        if (mlen == 0 || off + 2 + mlen > data.size()) break;
        spans.push_back({data.data() + off + 2, mlen});
        off += 2 + mlen;
    }

    // --- timed replay --------------------------------------------------------
    LatencyHist per_type[kNTypes];
    LatencyHist overall;
    OrderBook   book(n_messages);   // reserve up front so no rehash mid-measure

    using clock = std::chrono::steady_clock;
    for (const Span& s : spans) {
        itch::Message m;
        const auto t0 = clock::now();
        const bool ok = itch::decode(s.p, s.len, m);
        if (ok) book.apply(m);
        const auto t1 = clock::now();

        if (!ok) continue;
        const std::uint64_t ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        const int idx = index_of(m.type);
        if (idx < 0) continue;       // tape-only message: timed but not book-affecting
        per_type[idx].add(ns);
        overall.add(ns);
    }

    // --- emit JSON -----------------------------------------------------------
    std::ofstream  fout;
    std::ostream*  osp = &std::cout;
    if (out_path) {
        fout.open(out_path, std::ios::trunc);
        if (!fout) { std::cerr << "error: cannot write " << out_path << "\n"; return 1; }
        osp = &fout;
    }
    std::ostream& os = *osp;

    os << "{\n";
    // Optional ISO-8601 generation timestamp, supplied by CI via the environment
    // (the binary itself stays free of any clock/locale dependency in its output).
    if (const char* gen = std::getenv("HFTOB_GENERATED_AT"); gen && *gen)
        os << "  \"generated_at\": \"" << gen << "\",\n";
    os << "  \"environment\": \"GitHub Actions ubuntu-latest (virtualized; relative "
          "figures, not absolute)\",\n";
    os << "  \"method\": \"steady_clock per message; times decode + book.apply only "
          "(no IO/setup); deterministic synthetic ITCH 5.0, seed=" << kSeed << "\",\n";
    os << "  \"sample_messages\": " << overall.count() << ",\n";
    os << "  \"per_type\": [\n";
    bool first = true;
    for (std::size_t i = 0; i < kNTypes; ++i) {
        if (per_type[i].count() == 0) continue;   // skip types absent from this session
        if (!first) os << ",\n";
        first = false;
        os << "    { \"type\": \"" << kTypes[i].name << "\", \"code\": \""
           << kTypes[i].code << "\", ";
        emit_hist_fields(os, per_type[i]);
        os << " }";
    }
    os << "\n  ],\n";
    os << "  \"overall\": { ";
    emit_hist_fields(os, overall);
    os << " }\n";
    os << "}\n";

    if (out_path) std::cerr << "wrote latency JSON (" << overall.count()
                            << " book-affecting messages) to " << out_path << "\n";
    return 0;
}
