#pragma once

#if !defined(_WIN32)

#include <cstddef>
#include <cstdint>
#include <string>

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

/// Run one bridge session on an accepted connection until the peer sends `bye` or
/// closes. The protocol is strictly request/response so it is race-free without
/// any locking: the EA sends a tick and reads exactly one reply (`order` or
/// `nop`); if it was an order, the EA sends an `ack`, which the server reads
/// before the next tick. `Strategy` only needs `bool on_tick(const Tick&, Order&)`.
template <typename Strategy>
BridgeStats run_bridge(LineSocket& sock, Strategy& strat) {
    BridgeStats   st;
    std::uint64_t next_id = 1;
    std::string   line;

    while (sock.recv_line(line)) {
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
            case MsgKind::Unknown:
            default:
                break;  // ignore anything unexpected
        }
    }
    return st;
}

}  // namespace mt5
}  // namespace hftob

#endif  // !_WIN32
