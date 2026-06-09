#pragma once

#include <cstddef>
#include <cstdint>

#include "itch/decoder.hpp"
#include "itch/messages.hpp"

namespace hftob {

/// Walk a **BinaryFILE-framed** ITCH 5.0 byte stream — the format NASDAQ's
/// downloadable daily samples use, where every message is preceded by a 2-byte
/// **big-endian length**. For each message that decodes, `sink(const
/// itch::Message&)` is invoked. The framed length is authoritative (it covers
/// every message type, including variable-length ones), so this is the canonical
/// way to read both real captures and the synthetic generator's output. Stops at
/// a zero length (end-of-file padding) or a truncated tail; returns bytes consumed.
template <typename Sink>
std::size_t for_each_framed_message(const std::uint8_t* buf, std::size_t len, Sink&& sink) {
    std::size_t off = 0;
    while (off + 2 <= len) {
        const std::size_t mlen = (static_cast<std::size_t>(buf[off]) << 8) |
                                 static_cast<std::size_t>(buf[off + 1]);
        if (mlen == 0) break;                  // zero-length record = end/padding
        if (off + 2 + mlen > len) break;       // truncated tail
        itch::Message m;
        if (itch::decode(buf + off + 2, mlen, m)) sink(m);
        off += 2 + mlen;
    }
    return off;
}

}  // namespace hftob
