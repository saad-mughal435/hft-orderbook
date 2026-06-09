// obreplay — replay a NASDAQ ITCH 5.0 capture (or a synthetic stream) through the
// full decode -> book pipeline and report throughput + a latency distribution.
//
//   obreplay <capture.itch | capture.gz>   replay a captured ITCH 5.0 file
//   obreplay --synthetic <N>               replay N deterministically generated msgs
//
// Real full-day NASDAQ samples (≈3.5–5.6 GB) live at emi.nasdaq.com and are NOT
// committed; `gencap` produces a small reproducible capture, or use --synthetic.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "book/order_book.hpp"
#include "core/latency_hist.hpp"
#include "core/types.hpp"
#include "feed/pipeline.hpp"
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
              << "  obreplay <capture.itch | capture.gz>   replay an ITCH 5.0 capture\n"
              << "  obreplay --synthetic <N>               replay N generated messages\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::uint8_t> data;
    std::string               source;

    if (argc == 3 && std::string(argv[1]) == "--synthetic") {
        const long n = std::strtol(argv[2], nullptr, 10);
        if (n <= 0) { usage(); return 2; }
        data   = make_synthetic_itch(static_cast<std::size_t>(n));
        source = std::string("synthetic(") + argv[2] + ")";
    } else if (argc == 2 && argv[1][0] != '-') {
        if (!load_file(argv[1], data)) return 1;
        source = argv[1];
    } else {
        usage();
        return 2;
    }

    if (data.empty()) { std::cerr << "error: empty input\n"; return 1; }

    // Pass 1: timed end-to-end throughput — one clock pair, so no per-message
    // timer overhead pollutes the headline number.
    OrderBook  book;
    const auto t0 = Clock::now();
    const std::size_t n = replay_single_threaded(data.data(), data.size(), book);
    const auto t1 = Clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();

    // Pass 2: per-message decode+apply latency, for the tail shape. The two
    // clock reads per message add overhead (documented), but the distribution's
    // *shape* is still informative.
    LatencyHist hist;
    {
        OrderBook   b2;
        std::size_t off = 0;
        while (off < data.size()) {
            const std::size_t mlen = itch::message_length(static_cast<char>(data[off]));
            if (mlen == 0 || off + mlen > data.size()) break;
            itch::Message m;
            const auto s0 = Clock::now();
            if (itch::decode(data.data() + off, mlen, m)) b2.apply(m);
            const auto s1 = Clock::now();
            hist.add(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(s1 - s0).count()));
            off += mlen;
        }
    }

    Price      bid = 0, ask = 0;
    Qty        bq = 0, aq = 0;
    const bool hb = book.best_bid(bid, bq);
    const bool ha = book.best_ask(ask, aq);

    std::cout << "obreplay  source=" << source << "\n"
              << "  bytes        " << data.size() << "\n"
              << "  messages     " << n << "\n"
              << "  wall time    " << (ns / 1e6) << " ms\n"
              << "  throughput   " << (n / (ns / 1e9)) << " msg/s\n"
              << "  per message  " << (ns / static_cast<double>(n ? n : 1))
              << " ns (amortized)\n\n";

    std::cout << "final book:\n";
    if (hb) std::cout << "  best bid       " << to_dollars(bid) << " x " << bq << "\n";
    else    std::cout << "  best bid       (empty)\n";
    if (ha) std::cout << "  best ask       " << to_dollars(ask) << " x " << aq << "\n";
    else    std::cout << "  best ask       (empty)\n";
    std::cout << "  resting orders " << book.order_count() << "  ("
              << book.bid_levels() << " bid / " << book.ask_levels() << " ask levels)\n\n";

    hist.print(std::cout,
               "per-message decode+apply latency [includes timer overhead; see README]");
    return 0;
}
