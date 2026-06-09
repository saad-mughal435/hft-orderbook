#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "itch/decoder.hpp"
#include "itch/messages.hpp"

namespace hftob {

/// **MoldUDP64** is NASDAQ's UDP transport for the *real-time* market-data feed
/// (the historical files use BinaryFILE). A datagram = a 20-byte header (10-byte
/// session id, 8-byte big-endian sequence number, 2-byte message count) followed
/// by that many `[2-byte big-endian length][ITCH message]` blocks. One datagram
/// carries a burst of messages; the sequence number lets a receiver detect drops
/// on a lossy multicast feed. The per-message block framing is identical to
/// BinaryFILE, so a framed ITCH stream packetizes into MoldUDP64 directly.
struct MoldHeader {
    char          session[10] = {};
    std::uint64_t seq         = 0;
    std::uint16_t count       = 0;
};

constexpr std::uint16_t kMoldEndOfSession = 0xFFFF;

inline bool parse_mold_header(const std::uint8_t* buf, std::size_t len, MoldHeader& h) {
    if (len < 20) return false;
    for (int i = 0; i < 10; ++i) h.session[i] = static_cast<char>(buf[i]);
    std::uint64_t s = 0;
    for (int i = 0; i < 8; ++i) s = (s << 8) | buf[10 + i];
    h.seq   = s;
    h.count = static_cast<std::uint16_t>((static_cast<std::uint16_t>(buf[18]) << 8) | buf[19]);
    return true;
}

/// Build one MoldUDP64 datagram around pre-framed message blocks.
inline std::vector<std::uint8_t> encode_mold_packet(const std::string& session, std::uint64_t seq,
                                                    std::uint16_t count, const std::uint8_t* blocks,
                                                    std::size_t blen) {
    std::vector<std::uint8_t> p;
    p.reserve(20 + blen);
    for (int i = 0; i < 10; ++i)
        p.push_back(i < static_cast<int>(session.size()) ? static_cast<std::uint8_t>(session[i])
                                                         : static_cast<std::uint8_t>(' '));
    for (int i = 7; i >= 0; --i) p.push_back(static_cast<std::uint8_t>((seq >> (8 * i)) & 0xFF));
    p.push_back(static_cast<std::uint8_t>((count >> 8) & 0xFF));
    p.push_back(static_cast<std::uint8_t>(count & 0xFF));
    if (blen) p.insert(p.end(), blocks, blocks + blen);  // guard null blocks (EOS packets)
    return p;
}

/// Split a BinaryFILE-framed ITCH stream into MoldUDP64 datagrams of up to
/// `msgs_per_packet` messages each (sequence numbers start at 1).
inline std::vector<std::vector<std::uint8_t>> packetize_mold(const std::string& session,
                                                            const std::uint8_t* framed,
                                                            std::size_t len,
                                                            std::uint16_t msgs_per_packet = 8) {
    std::vector<std::vector<std::uint8_t>> out;
    std::uint64_t                          seq = 1;
    std::size_t                            off = 0;
    while (off < len) {
        const std::size_t start = off;
        std::uint16_t     cnt   = 0;
        while (off + 2 <= len && cnt < msgs_per_packet) {
            const std::size_t mlen = (static_cast<std::size_t>(framed[off]) << 8) | framed[off + 1];
            if (mlen == 0 || off + 2 + mlen > len) break;
            off += 2 + mlen;
            ++cnt;
        }
        if (cnt == 0) break;
        out.push_back(encode_mold_packet(session, seq, cnt, framed + start, off - start));
        seq += cnt;
    }
    return out;
}

/// Tracks sequence continuity across datagrams - drop/gap detection, the central
/// concern of a UDP multicast receiver. `sink(seq, message)` is called per message.
class MoldSession {
public:
    template <typename Sink>
    void on_packet(const std::uint8_t* buf, std::size_t len, Sink&& sink) {
        MoldHeader h;
        if (!parse_mold_header(buf, len, h)) return;
        ++packets_;
        if (h.count == kMoldEndOfSession) { ended_ = true; return; }
        if (started_ && h.seq > expected_) gaps_ += (h.seq - expected_);  // dropped messages
        started_ = true;

        std::uint64_t seq = h.seq;
        std::size_t   off = 20;
        for (std::uint16_t i = 0; i < h.count; ++i) {
            if (off + 2 > len) break;
            const std::size_t mlen = (static_cast<std::size_t>(buf[off]) << 8) | buf[off + 1];
            off += 2;
            if (off + mlen > len) break;
            itch::Message m;
            if (itch::decode(buf + off, mlen, m)) {
                sink(seq, m);
                ++messages_;
            }
            ++seq;
            off += mlen;
        }
        expected_ = seq;
    }

    std::uint64_t expected_seq() const { return expected_; }
    std::uint64_t gaps() const { return gaps_; }
    std::uint64_t packets() const { return packets_; }
    std::uint64_t messages() const { return messages_; }
    bool          ended() const { return ended_; }

private:
    std::uint64_t expected_ = 0;
    std::uint64_t gaps_     = 0;
    std::uint64_t packets_  = 0;
    std::uint64_t messages_ = 0;
    bool          started_  = false;
    bool          ended_    = false;
};

}  // namespace hftob
