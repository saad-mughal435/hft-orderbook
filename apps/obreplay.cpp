// obreplay — replay a NASDAQ ITCH 5.0 capture (or a synthetic stream) through the
// full decode -> multi-symbol book pipeline and report throughput, per-symbol
// top-of-book + depth, and a latency distribution.
//
//   obreplay <capture.itch | capture.gz> [--symbol SYM]
//   obreplay --synthetic <N>             [--symbol SYM]
//
// Real full-day NASDAQ samples (≈3.5–5.6 GB) live at emi.nasdaq.com and are NOT
// committed; `gencap` produces a small reproducible capture, or use --synthetic.
// Captures are BinaryFILE-framed (2-byte big-endian length per message). A real
// day interleaves thousands of symbols, routed by stock_locate into a BookSet.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "book/book_set.hpp"
#include "book/order_book.hpp"
#include "core/latency_hist.hpp"
#include "core/types.hpp"
#include "feed/framing.hpp"
#include "feed/synthetic.hpp"
#include "itch/decoder.hpp"
#include "itch/messages.hpp"

#ifdef HFTOB_HAVE_ZLIB
#include <zlib.h>
#endif

using namespace hftob;
using Clock = std::chrono::steady_clock;

namespace {

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() &&
           s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool load_file(const std::string& path, std::vector<std::uint8_t>& out) {
    if (ends_with(path, ".gz")) {
#ifdef HFTOB_HAVE_ZLIB
        gzFile f = gzopen(path.c_str(), "rb");
        if (!f) { std::cerr << "error: cannot open " << path << "\n"; return false; }
        char buf[1 << 16];
        int  n;
        while ((n = gzread(f, buf, sizeof(buf))) > 0)
            out.insert(out.end(), buf, buf + n);
        gzclose(f);
        return true;
#else
        std::cerr << "error: " << path << " is gzip but this build has no zlib.\n"
                  << "       gunzip it first, or rebuild with zlib available.\n";
        return false;
#endif
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "error: cannot open " << path << "\n"; return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

void usage() {
    std::cerr << "usage:\n"
              << "  obreplay <capture.itch | capture.gz> [--symbol SYM]\n"
              << "  obreplay --synthetic <N>             [--symbol SYM]\n";
}

std::string label(const BookSet& bs, std::uint16_t locate) {
    const std::string s = bs.symbol(locate);
    return s.empty() ? ("locate " + std::to_string(locate)) : s;
}

void print_top(const BookSet& bs, std::uint16_t locate) {
    const OrderBook* ob = bs.book(locate);
    if (!ob) return;
    Price bp = 0, ap = 0;
    Qty   bq = 0, aq = 0;
    const bool hb = ob->best_bid(bp, bq);
    const bool ha = ob->best_ask(ap, aq);
    std::cout << "  [" << label(bs, locate) << "]  ";
    if (hb) std::cout << "bid " << to_dollars(bp) << " x " << bq;
    else    std::cout << "bid (empty)";
    std::cout << "  |  ";
    if (ha) std::cout << "ask " << to_dollars(ap) << " x " << aq;
    else    std::cout << "ask (empty)";
    std::cout << "   (" << ob->order_count() << " orders, " << ob->bid_levels()
              << " bid / " << ob->ask_levels() << " ask levels)\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::uint8_t> data;
    std::string               source;
    std::string               focus;     // --symbol filter
    long                      synth = 0;
    std::string               file;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--symbol" && i + 1 < argc) {
            focus = argv[++i];
        } else if (a == "--synthetic" && i + 1 < argc) {
            synth = std::strtol(argv[++i], nullptr, 10);
        } else if (!a.empty() && a[0] != '-') {
            file = a;
        } else {
            usage();
            return 2;
        }
    }

    if (synth > 0) {
        data   = make_synthetic_itch(static_cast<std::size_t>(synth));
        source = "synthetic(" + std::to_string(synth) + ")";
    } else if (!file.empty()) {
        if (!load_file(file, data)) return 1;
        source = file;
    } else {
        usage();
        return 2;
    }

    if (data.empty()) { std::cerr << "error: empty input\n"; return 1; }

    // Pass 1: timed end-to-end throughput — one clock pair, no per-message timer
    // overhead in the headline number. Routed multi-symbol through a BookSet.
    BookSet    book;
    std::size_t n = 0;
    const auto t0 = Clock::now();
    for_each_framed_message(data.data(), data.size(),
                            [&](const itch::Message& m) { book.apply(m); ++n; });
    const auto t1 = Clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();

    // Pass 2: per-message decode+apply latency, for the tail shape. The two clock
    // reads per message add overhead (documented), but the distribution's *shape*
    // is still informative.
    LatencyHist hist;
    {
        BookSet     b2;
        std::size_t off = 0;
        while (off + 2 <= data.size()) {
            const std::size_t mlen = (static_cast<std::size_t>(data[off]) << 8) |
                                     static_cast<std::size_t>(data[off + 1]);
            if (mlen == 0 || off + 2 + mlen > data.size()) break;
            itch::Message m;
            const auto s0 = Clock::now();
            if (itch::decode(data.data() + off + 2, mlen, m)) b2.apply(m);
            const auto s1 = Clock::now();
            hist.add(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(s1 - s0).count()));
            off += 2 + mlen;
        }
    }

    std::cout << "obreplay  source=" << source << "\n"
              << "  bytes        " << data.size() << "\n"
              << "  messages     " << n << "\n"
              << "  wall time    " << (ns / 1e6) << " ms\n"
              << "  throughput   " << (n / (ns / 1e9)) << " msg/s\n"
              << "  per message  " << (ns / static_cast<double>(n ? n : 1))
              << " ns (amortized)\n\n";

    std::cout << "books: " << book.book_count() << " symbol(s), "
              << book.total_orders() << " resting orders\n";

    // Rank symbols by resting-order count and show the busiest.
    std::vector<std::pair<std::uint16_t, std::size_t>> ranked;
    for (const auto& kv : book.books())
        ranked.emplace_back(kv.first, kv.second.order_count());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    const std::size_t show = std::min<std::size_t>(ranked.size(), 10);
    if (show) std::cout << "\ntop symbols by resting orders:\n";
    for (std::size_t i = 0; i < show; ++i) print_top(book, ranked[i].first);

    // Pick a focal book: --symbol if given (by ticker or locate), else the busiest.
    std::uint16_t focus_locate = 0;
    bool          have_focus   = false;
    if (!focus.empty()) {
        for (const auto& kv : book.books())
            if (book.symbol(kv.first) == focus) { focus_locate = kv.first; have_focus = true; break; }
        if (!have_focus) {
            char*      end = nullptr;
            const long L   = std::strtol(focus.c_str(), &end, 10);
            if (end != focus.c_str() && book.book(static_cast<std::uint16_t>(L))) {
                focus_locate = static_cast<std::uint16_t>(L);
                have_focus   = true;
            }
        }
        if (!have_focus)
            std::cout << "\n(symbol \"" << focus << "\" not found; showing the busiest)\n";
    }
    if (!have_focus && !ranked.empty()) { focus_locate = ranked.front().first; have_focus = true; }

    if (have_focus) {
        const OrderBook* ob = book.book(focus_locate);
        std::cout << "\ndepth (best 5) for " << label(book, focus_locate) << ":\n  bids:";
        const auto bids = ob->bids(5);
        for (const auto& l : bids) std::cout << "  " << to_dollars(l.first) << "x" << l.second;
        if (bids.empty()) std::cout << "  (empty)";
        std::cout << "\n  asks:";
        const auto asks = ob->asks(5);
        for (const auto& l : asks) std::cout << "  " << to_dollars(l.first) << "x" << l.second;
        if (asks.empty()) std::cout << "  (empty)";
        std::cout << "\n";
    }
    std::cout << "\n";

    hist.print(std::cout,
               "per-message decode+apply latency [includes timer overhead; see README]");
    return 0;
}
