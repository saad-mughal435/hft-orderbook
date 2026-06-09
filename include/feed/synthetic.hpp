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
        } else if (roll < 70) {                  // execute
            const std::size_t i = pick();
            hdr(m, 'E', 1, ts);
            be_n(m, live[i].first, 8);
            be32(m, static_cast<std::uint32_t>(1 + pct(20)));
            be_n(m, emitted, 8);                 // match number
            frame();
        } else if (roll < 82) {                  // cancel (partial)
            const std::size_t i = pick();
            hdr(m, 'X', 1, ts);
            be_n(m, live[i].first, 8);
            be32(m, static_cast<std::uint32_t>(1 + pct(20)));
            frame();
        } else if (roll < 92) {                  // delete
            const std::size_t i = pick();
            hdr(m, 'D', 1, ts);
            be_n(m, live[i].first, 8);
            frame();
            live[i] = live.back();               // swap-pop
            live.pop_back();
        } else {                                 // replace (reprice/resize)
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
        }
        ts += 1 + static_cast<std::uint64_t>(pct(1000));
        ++emitted;
    }
    return b;
}

}  // namespace hftob
