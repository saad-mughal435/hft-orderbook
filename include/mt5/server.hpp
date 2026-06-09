#pragma once

#if !defined(_WIN32)

#include <cstddef>
#include <cstdint>
#include <string>

#include "book/metrics.hpp"
#include "mt5/protocol.hpp"
#include "mt5/tcp.hpp"

namespace hftob {
namespace mt5 {

struct BridgeStats {
    std::size_t ticks  = 0;  // ticks received from the EA
    std::size_t orders = 0;  // orders the strategy emitted back to the EA
    std::size_t acks   = 0;  // trade acks the EA returned
    std::size_t nops   = 0;  // ticks that produced no order
};

/// Run one bridge session on an accepted connection until the peer sends `bye`,
/// closes, or (if `idle_timeout_ms`/`max_idle_intervals` are set) goes silent for
/// `max_idle_intervals` consecutive timeouts — so a dead EA can't pin the session
/// forever. Any received message (including a heartbeat) resets the idle counter.
///
/// The protocol is strictly request/response so it is race-free without locking:
/// the EA sends a tick and reads exactly one reply (`order` or `nop`); if it was
/// an order, the EA sends an `ack`, which the server reads before the next tick.
/// `Strategy` only needs `bool on_tick(const Tick&, Order&)`.
template <typename Strategy>
BridgeStats run_bridge(LineSocket& sock, Strategy& strat,
                       int idle_timeout_ms = 0, int max_idle_intervals = 0) {
    BridgeStats   st;
    std::uint64_t next_id = 1;
    std::string   line;

    if (idle_timeout_ms > 0) sock.set_recv_timeout(idle_timeout_ms);
    int idle = 0;

    for (;;) {
        if (!sock.recv_line(line)) {
            if (sock.timed_out() && max_idle_intervals > 0) {
                if (++idle >= max_idle_intervals) break;  // peer silent too long
                continue;
            }
            break;  // peer closed (or no idle timeout configured)
        }
        idle = 0;  // any traffic (incl. heartbeat) keeps the session alive

        switch (kind_of(line)) {
            case MsgKind::Hello:
                sock.send_line(encode_hello_reply("mt5d", true));
                break;

            case MsgKind::Subscribe:
                break;  // accepted silently (single-symbol bridge)

            case MsgKind::Tick: {
                Tick t;
                if (!parse_tick(line, t)) break;
                ++st.ticks;
                Order ord;
                if (strat.on_tick(t, ord)) {
                    ord.id = next_id++;
                    sock.send_line(encode(ord));
                    ++st.orders;
                } else {
                    sock.send_line(encode_nop());
                    ++st.nops;
                }
                break;
            }

            case MsgKind::Ack:
                ++st.acks;
                break;

            case MsgKind::Bye:
                return st;

            case MsgKind::Heartbeat:
            case MsgKind::Order:
            case MsgKind::Nop:
            case MsgKind::Depth:
            case MsgKind::Signal:
            case MsgKind::Unknown:
            default:
                break;  // ignore anything unexpected
        }
    }
    return st;
}

/// Publish a top-`n` depth snapshot of `book` for `symbol` to a connected client —
/// how the ITCH-reconstructed book streams out to a MetaTrader/other subscriber.
/// Templated on the book type to keep this header decoupled from `book/`.
template <typename Book>
bool publish_depth(LineSocket& sock, const std::string& symbol, const Book& book,
                   std::size_t n) {
    return sock.send_line(encode_depth(symbol, book.bids(n), book.asks(n)));
}

/// Publish the engine's microstructure signal (mid / micro-price / imbalance /
/// spread bps) for `symbol`, computed from the reconstructed `book`.
template <typename Book>
bool publish_signal(LineSocket& sock, const std::string& symbol, const Book& book,
                    std::size_t n = 5) {
    const BookMetrics m = compute_metrics(book, n);
    return sock.send_line(
        encode_signal(symbol, m.mid, m.microprice, m.imbalance_top, m.spread_bps));
}

}  // namespace mt5
}  // namespace hftob

#endif  // !_WIN32
