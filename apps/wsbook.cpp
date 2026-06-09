// wsbook — replay an ITCH capture/synthetic and stream conflated L2 depth +
// microstructure signals as JSON. Two modes:
//
//   wsbook --dump <N> [--synthetic M] [--symbols K] [--symbol SYM]
//       Print N evenly-spaced JSON snapshots (NDJSON) to stdout — used to *record*
//       a real engine replay for the static browser viewer, and as a CI smoke.
//
//   wsbook [port] [--synthetic M] [--symbols K] [--symbol SYM] [--frames N]
//       Serve WebSocket clients (default 127.0.0.1:9010); stream snapshots as they
//       reconstruct. --frames N sends N then closes. Pair with site/hft-book/viewer.html.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "book/book_set.hpp"
#include "book/metrics.hpp"
#include "book/order_book.hpp"
#include "core/types.hpp"
#include "feed/framing.hpp"
#include "feed/synthetic.hpp"
#include "feed/websocket.hpp"
#include "itch/decoder.hpp"
#include "itch/messages.hpp"
#include "mt5/tcp.hpp"

using namespace hftob;

namespace {

std::string fmt(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.5g", v);
    return std::string(b);
}

std::string snapshot_json(const std::string& sym, const OrderBook& ob, std::size_t seq) {
    const auto bids = ob.bids(10);
    const auto asks = ob.asks(10);
    const auto mx   = compute_metrics(ob, 5);
    std::string s = "{\"symbol\":\"" + sym + "\",\"seq\":" + std::to_string(seq) + ",\"bids\":[";
    for (std::size_t i = 0; i < bids.size(); ++i) {
        if (i) s.push_back(',');
        s += "[" + fmt(to_dollars(bids[i].first)) + "," + std::to_string(bids[i].second) + "]";
    }
    s += "],\"asks\":[";
    for (std::size_t i = 0; i < asks.size(); ++i) {
        if (i) s.push_back(',');
        s += "[" + fmt(to_dollars(asks[i].first)) + "," + std::to_string(asks[i].second) + "]";
    }
    s += "],\"mid\":" + fmt(mx.mid) + ",\"microprice\":" + fmt(mx.microprice) +
         ",\"imbalance\":" + fmt(mx.imbalance_top) + ",\"spread_bps\":" + fmt(mx.spread_bps) + "}";
    return s;
}

std::size_t count_messages(const std::vector<std::uint8_t>& d) {
    std::size_t n = 0;
    for_each_framed_message(d.data(), d.size(), [&](const itch::Message&) { ++n; });
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    long        port = 9010, dump = 0, frames = 0, synth = 20000, symbols = 1;
    std::string focus;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dump" && i + 1 < argc)            dump = std::strtol(argv[++i], nullptr, 10);
        else if (a == "--synthetic" && i + 1 < argc)  synth = std::strtol(argv[++i], nullptr, 10);
        else if (a == "--symbols" && i + 1 < argc)    symbols = std::strtol(argv[++i], nullptr, 10);
        else if (a == "--frames" && i + 1 < argc)     frames = std::strtol(argv[++i], nullptr, 10);
        else if (a == "--symbol" && i + 1 < argc)     focus = argv[++i];
        else {
            char*      e = nullptr;
            const long p = std::strtol(a.c_str(), &e, 10);
            if (e != a.c_str() && *e == '\0') port = p;
        }
    }
    if (symbols < 1) symbols = 1;
    if (synth < 1)   synth = 1;

    const std::vector<std::uint8_t> data =
        (symbols > 1) ? make_synthetic_multi(static_cast<std::size_t>(synth), static_cast<unsigned>(symbols))
                      : make_synthetic_itch(static_cast<std::size_t>(synth));

    auto focal_locate = [&](const BookSet& bs) -> std::uint16_t {
        if (!focus.empty())
            for (const auto& kv : bs.books())
                if (bs.symbol(kv.first) == focus) return kv.first;
        return 1;
    };
    auto focal_name = [&](const BookSet& bs, std::uint16_t loc) {
        std::string name = bs.symbol(loc);
        return name.empty() ? std::string("SYNTH") : name;
    };

    // Walk the stream, emitting a snapshot of the focal book every `every` messages.
    auto stream = [&](std::size_t target, auto&& emit) {
        const std::size_t total = count_messages(data);
        const std::size_t every = total / (target + 1) + 1;
        BookSet           book;
        std::size_t       applied = 0, sent = 0, off = 0;
        while (off + 2 <= data.size() && sent < target) {
            const std::size_t mlen =
                (static_cast<std::size_t>(data[off]) << 8) | static_cast<std::size_t>(data[off + 1]);
            if (mlen == 0 || off + 2 + mlen > data.size()) break;
            itch::Message m;
            if (itch::decode(data.data() + off + 2, mlen, m)) book.apply(m);
            ++applied;
            off += 2 + mlen;
            if (applied % every == 0) {
                const std::uint16_t loc = focal_locate(book);
                const OrderBook*    ob  = book.book(loc);
                if (ob) {
                    if (!emit(snapshot_json(focal_name(book, loc), *ob, sent))) return;
                    ++sent;
                }
            }
        }
    };

    if (dump > 0) {  // record N snapshots to stdout (NDJSON)
        stream(static_cast<std::size_t>(dump), [](const std::string& j) {
            std::cout << j << "\n";
            return true;
        });
        return 0;
    }

    try {
        mt5::Listener listener(static_cast<std::uint16_t>(port));
        std::cerr << "wsbook on 127.0.0.1:" << listener.port() << " (Ctrl-C to stop)\n";
        for (;;) {
            mt5::LineSocket conn = listener.accept();
            if (!ws::handshake(conn)) continue;
            std::cerr << "viewer connected\n";
            const std::size_t target = (frames > 0) ? static_cast<std::size_t>(frames) : 120;
            stream(target, [&](const std::string& j) { return ws::send_text(conn, j); });
        }
    } catch (const std::exception& e) {
        std::cerr << "wsbook error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
