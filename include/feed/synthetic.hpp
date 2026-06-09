#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "core/types.hpp"

namespace hftob {
namespace detail {

inline void be16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
inline void be32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 24));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
inline void be_n(std::vector<std::uint8_t>& b, std::uint64_t v, int n) {
    for (int s = (n - 1) * 8; s >= 0; s -= 8)
        b.push_back(static_cast<std::uint8_t>(v >> s));
}
inline void hdr(std::vector<std::uint8_t>& b, char type, std::uint16_t locate,
                std::uint64_t ts) {
    b.push_back(static_cast<std::uint8_t>(type));
    be16(b, locate);
    be16(b, 0);        // tracking number
    be_n(b, ts, 6);    // 6-byte nanosecond timestamp
}
inline void stock8(std::vector<std::uint8_t>& b) {
    const char sym[8] = {'S', 'Y', 'N', 'T', 'H', ' ', ' ', ' '};
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(sym[i]));
}

}  // namespace detail

/// Build a deterministic synthetic NASDAQ ITCH 5.0 byte stream of `n_messages`
/// messages: a random walk that adds resting orders and then executes / cancels /
/// deletes / replaces them against a live pool, so the `order_ref` map stays
/// populated — realistic load for benchmarking the hot path. The same `seed`
/// always yields identical bytes, so benchmarks and the replay demo are
/// reproducible. The stream is **BinaryFILE-framed** (each message preceded by a
/// 2-byte big-endian length), matching real NASDAQ daily captures and read by
/// `for_each_framed_message`.
inline std::vector<std::uint8_t> make_synthetic_itch(std::size_t n_messages,
                                                     std::uint32_t seed = 1) {
    using namespace detail;
    std::vector<std::uint8_t> b;
    b.reserve(n_messages * 38);          // ~36-byte body + 2-byte length prefix

    std::vector<std::uint8_t> m;         // scratch for one message body
    m.reserve(64);
    auto frame = [&]() {                 // emit BinaryFILE 2-byte BE length + body
        be16(b, static_cast<std::uint16_t>(m.size()));
        b.insert(b.end(), m.begin(), m.end());
        m.clear();
    };

    std::mt19937 rng(seed);
    auto pct = [&](int hi) {
        return static_cast<int>(rng() % static_cast<unsigned>(hi));
    };

    std::vector<std::pair<std::uint64_t, char>> live;  // (order_ref, side)
    live.reserve(4096);

    std::uint64_t next_ref = 1;
    std::uint64_t ts       = 34200ull * 1000000000ull;  // ~09:30:00 in ns
    std::int64_t  mid      = 1000000;                    // $100.0000 in price ticks

    auto add_px = [&](char side) {
        const std::int64_t off = (side == 'B') ? -(1 + pct(50)) : (1 + pct(50));
        std::int64_t px = mid + off * 100;
        return px < 100 ? std::int64_t{100} : px;
    };
    auto emit_add = [&]() {
        const char          side   = (pct(2) == 0) ? 'B' : 'S';
        const std::uint64_t ref    = next_ref++;
        const std::uint32_t shares = static_cast<std::uint32_t>(10 + pct(990));
        const std::int64_t  px     = add_px(side);
        hdr(m, 'A', 1, ts);
        be_n(m, ref, 8);
        m.push_back(static_cast<std::uint8_t>(side));
        be32(m, shares);
        stock8(m);
        be32(m, static_cast<std::uint32_t>(px));
        frame();
        live.emplace_back(ref, side);
    };
    auto pick = [&]() { return static_cast<std::size_t>(rng() % live.size()); };

    std::size_t emitted = 0;
    for (int i = 0; i < 16 && emitted < n_messages; ++i) {  // seed a pool to mutate
        emit_add();
        ++emitted;
    }

    while (emitted < n_messages) {
        if (pct(8) == 0) {                       // occasionally walk the mid
            mid += (pct(2) ? 100 : -100);
            if (mid < 1000) mid = 1000;
        }
        const int roll = pct(100);
        if (live.size() < 8 || roll < 50) {      // ~50% adds (keeps the book deep)
            emit_add();
        } else if (roll < 66) {                  // execute
            const std::size_t i = pick();
            hdr(m, 'E', 1, ts);
            be_n(m, live[i].first, 8);
            be32(m, static_cast<std::uint32_t>(1 + pct(20)));
            be_n(m, emitted, 8);                 // match number
            frame();
        } else if (roll < 78) {                  // cancel (partial)
            const std::size_t i = pick();
            hdr(m, 'X', 1, ts);
            be_n(m, live[i].first, 8);
            be32(m, static_cast<std::uint32_t>(1 + pct(20)));
            frame();
        } else if (roll < 88) {                  // delete
            const std::size_t i = pick();
            hdr(m, 'D', 1, ts);
            be_n(m, live[i].first, 8);
            frame();
            live[i] = live.back();               // swap-pop
            live.pop_back();
        } else if (roll < 96) {                  // replace (reprice/resize)
            const std::size_t i    = pick();
            const char        side = live[i].second;
            const std::uint64_t nref = next_ref++;
            hdr(m, 'U', 1, ts);
            be_n(m, live[i].first, 8);           // original ref
            be_n(m, nref, 8);                    // new ref
            be32(m, static_cast<std::uint32_t>(10 + pct(990)));
            be32(m, static_cast<std::uint32_t>(add_px(side)));
            frame();
            live[i] = {nref, side};
        } else {                                 // trade ('P') — tape-only, no book change
            const char side = (pct(2) == 0) ? 'B' : 'S';
            hdr(m, 'P', 1, ts);
            be_n(m, 0, 8);                       // order_ref (unused by the tape)
            m.push_back(static_cast<std::uint8_t>(side));
            be32(m, static_cast<std::uint32_t>(1 + pct(200)));   // shares
            stock8(m);
            be32(m, static_cast<std::uint32_t>(mid));            // price at the mid
            be_n(m, emitted, 8);                 // match number -> len 44
            frame();
        }
        ts += 1 + static_cast<std::uint64_t>(pct(1000));
        ++emitted;
    }
    return b;
}

/// Build a deterministic **multi-symbol** synthetic ITCH 5.0 stream: a
/// StockDirectory (`R`) per symbol (locates 1..n_symbols, named `SYNnn`) followed
/// by orders interleaved across all symbols, each carrying its symbol's
/// `stock_locate`. This makes the multi-symbol and sharded paths demonstrable in
/// CI and `obreplay` without a multi-GB real capture. BinaryFILE-framed.
inline std::vector<std::uint8_t> make_synthetic_multi(std::size_t n_messages,
                                                      unsigned n_symbols,
                                                      std::uint32_t seed = 1) {
    using namespace detail;
    if (n_symbols < 1) n_symbols = 1;

    std::vector<std::uint8_t> b;
    b.reserve(n_messages * 38);
    std::vector<std::uint8_t> m;
    m.reserve(64);
    auto frame = [&]() {
        be16(b, static_cast<std::uint16_t>(m.size()));
        b.insert(b.end(), m.begin(), m.end());
        m.clear();
    };

    std::mt19937 rng(seed);
    auto pct = [&](int hi) { return static_cast<int>(rng() % static_cast<unsigned>(hi)); };

    struct Sym {
        std::int64_t                                mid;
        std::vector<std::pair<std::uint64_t, char>> live;
    };
    std::vector<Sym> syms(n_symbols);
    for (unsigned s = 0; s < n_symbols; ++s)
        syms[s].mid = 1000000 + static_cast<std::int64_t>(s) * 5000;  // distinct mids

    std::uint64_t next_ref = 1;
    std::uint64_t ts       = 34200ull * 1000000000ull;
    std::size_t   emitted  = 0;

    // One StockDirectory per symbol so the books can be named.
    for (unsigned s = 0; s < n_symbols && emitted < n_messages; ++s) {
        hdr(m, 'R', static_cast<std::uint16_t>(s + 1), ts);
        char sym[8] = {'S', 'Y', 'N', ' ', ' ', ' ', ' ', ' '};
        sym[3] = static_cast<char>('0' + (s / 10) % 10);
        sym[4] = static_cast<char>('0' + s % 10);
        for (int i = 0; i < 8; ++i) m.push_back(static_cast<std::uint8_t>(sym[i]));
        while (m.size() < 39) m.push_back(0);   // pad to the 39-byte 'R' length
        frame();
        ++emitted;
    }

    while (emitted < n_messages) {
        const unsigned      s      = static_cast<unsigned>(rng() % n_symbols);
        const std::uint16_t locate = static_cast<std::uint16_t>(s + 1);
        Sym&                sym    = syms[s];
        if (pct(8) == 0) {
            sym.mid += (pct(2) ? 100 : -100);
            if (sym.mid < 1000) sym.mid = 1000;
        }
        auto add_px = [&](char side) {
            const std::int64_t off = (side == 'B') ? -(1 + pct(50)) : (1 + pct(50));
            std::int64_t       px  = sym.mid + off * 100;
            return px < 100 ? std::int64_t{100} : px;
        };

        const int roll = pct(100);
        if (sym.live.size() < 4 || roll < 55) {       // add
            const char          side = (pct(2) == 0) ? 'B' : 'S';
            const std::uint64_t ref  = next_ref++;
            hdr(m, 'A', locate, ts);
            be_n(m, ref, 8);
            m.push_back(static_cast<std::uint8_t>(side));
            be32(m, static_cast<std::uint32_t>(10 + pct(990)));
            stock8(m);
            be32(m, static_cast<std::uint32_t>(add_px(side)));
            frame();
            sym.live.emplace_back(ref, side);
        } else if (roll < 72) {                        // execute
            const std::size_t i = static_cast<std::size_t>(rng() % sym.live.size());
            hdr(m, 'E', locate, ts);
            be_n(m, sym.live[i].first, 8);
            be32(m, static_cast<std::uint32_t>(1 + pct(20)));
            be_n(m, emitted, 8);
            frame();
        } else if (roll < 84) {                        // cancel
            const std::size_t i = static_cast<std::size_t>(rng() % sym.live.size());
            hdr(m, 'X', locate, ts);
            be_n(m, sym.live[i].first, 8);
            be32(m, static_cast<std::uint32_t>(1 + pct(20)));
            frame();
        } else {                                       // delete
            const std::size_t i = static_cast<std::size_t>(rng() % sym.live.size());
            hdr(m, 'D', locate, ts);
            be_n(m, sym.live[i].first, 8);
            frame();
            sym.live[i] = sym.live.back();
            sym.live.pop_back();
        }
        ts += 1 + static_cast<std::uint64_t>(pct(1000));
        ++emitted;
    }
    return b;
}

/// Build a deterministic synthetic ITCH 5.0 stream that reconstructs into a
/// **realistic, never-crossed** limit-order book — a clean L2 ladder around a
/// tight spread, with churning depth and trade prints at the mid. Unlike
/// `make_synthetic_itch` (a stress mix that lets resting orders pile up across the
/// mid, since the engine is a reconstructor, not a matcher), every add here
/// respects the current inside: a new bid is priced strictly below the best ask
/// and a new ask strictly above the best bid, so `best_bid < best_ask` always
/// holds. Used for the live L2 viewer demo. Single symbol (locate 1).
inline std::vector<std::uint8_t> make_synthetic_book(std::size_t n_messages,
                                                     std::uint32_t seed = 1) {
    using namespace detail;
    std::vector<std::uint8_t> b;
    b.reserve(n_messages * 38);
    std::vector<std::uint8_t> m;
    m.reserve(64);
    auto frame = [&]() {
        be16(b, static_cast<std::uint16_t>(m.size()));
        b.insert(b.end(), m.begin(), m.end());
        m.clear();
    };
    std::mt19937 rng(seed);
    auto pct = [&](int hi) { return static_cast<int>(rng() % static_cast<unsigned>(hi)); };

    constexpr std::int64_t C        = 100;       // one cent, in ticks
    const std::int64_t     seed_bid = 1001000;   // $100.10
    const std::int64_t     seed_ask = 1001100;   // $100.11

    std::vector<std::pair<std::uint64_t, char>> live;
    std::vector<std::int64_t>                   px;   // price per live order (parallel)
    std::uint64_t                               next_ref = 1;
    std::uint64_t                               ts       = 34200ull * 1000000000ull;

    auto best = [&](char side) -> std::int64_t {
        bool         any = false;
        std::int64_t v   = 0;
        for (std::size_t i = 0; i < live.size(); ++i)
            if (live[i].second == side) {
                if (!any) { v = px[i]; any = true; }
                else if (side == 'B' ? (px[i] > v) : (px[i] < v)) { v = px[i]; }
            }
        return any ? v : (side == 'B' ? seed_bid : seed_ask);
    };
    auto add = [&](char side) {
        const std::int64_t bb = best('B'), ba = best('S');
        std::int64_t       price = (side == 'B') ? (ba - C - static_cast<std::int64_t>(pct(10)) * C)
                                                 : (bb + C + static_cast<std::int64_t>(pct(10)) * C);
        if (price < C) price = C;
        const std::uint64_t ref = next_ref++;
        hdr(m, 'A', 1, ts);
        be_n(m, ref, 8);
        m.push_back(static_cast<std::uint8_t>(side));
        be32(m, static_cast<std::uint32_t>(50 + pct(2500)));
        stock8(m);
        be32(m, static_cast<std::uint32_t>(price));
        frame();
        live.emplace_back(ref, side);
        px.push_back(price);
    };

    std::size_t emitted = 0;
    for (int i = 0; i < 20 && emitted < n_messages; ++i) {
        add((i & 1) ? 'S' : 'B');
        ++emitted;
    }

    while (emitted < n_messages) {
        const int roll = pct(100);
        if (roll < 42 || live.size() < 10) {
            add((pct(2) == 0) ? 'B' : 'S');
        } else if (roll < 64) {                          // execute (partial reduce)
            const std::size_t i = static_cast<std::size_t>(rng() % live.size());
            hdr(m, 'E', 1, ts);
            be_n(m, live[i].first, 8);
            be32(m, static_cast<std::uint32_t>(5 + pct(150)));
            be_n(m, emitted, 8);
            frame();
        } else if (roll < 82) {                          // delete
            const std::size_t i = static_cast<std::size_t>(rng() % live.size());
            hdr(m, 'D', 1, ts);
            be_n(m, live[i].first, 8);
            frame();
            live[i] = live.back();
            live.pop_back();
            px[i] = px.back();
            px.pop_back();
        } else {                                         // trade ('P') at the mid
            const std::int64_t mid = (best('B') + best('S')) / 2;
            hdr(m, 'P', 1, ts);
            be_n(m, 0, 8);
            m.push_back(static_cast<std::uint8_t>((pct(2) == 0) ? 'B' : 'S'));
            be32(m, static_cast<std::uint32_t>(10 + pct(600)));
            stock8(m);
            be32(m, static_cast<std::uint32_t>(mid));
            be_n(m, emitted, 8);
            frame();
        }
        ts += 1 + static_cast<std::uint64_t>(pct(800));
        ++emitted;
    }
    return b;
}

}  // namespace hftob
