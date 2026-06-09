#pragma once

#if !defined(_WIN32)

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/sha1.hpp"
#include "mt5/tcp.hpp"

namespace hftob {
namespace ws {

/// `Sec-WebSocket-Accept` per RFC 6455: base64(sha1(key + magic GUID)).
inline std::string accept_key(const std::string& client_key) {
    static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const auto        d = sha1(client_key + kGuid);
    return base64(d.data(), d.size());
}

/// Server side: read the HTTP upgrade request, extract `Sec-WebSocket-Key`, and
/// send the `101 Switching Protocols` response. Returns false if it isn't a valid
/// WebSocket upgrade.
inline bool handshake(mt5::LineSocket& sock) {
    std::string key, line;
    for (;;) {
        if (!sock.recv_line(line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;  // blank line: end of headers
        const std::string h = "sec-websocket-key:";
        if (line.size() >= h.size()) {
            std::string lower = line.substr(0, h.size());
            for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (lower == h) {
                std::string v = line.substr(line.find(':') + 1);
                const std::size_t a = v.find_first_not_of(" \t");
                const std::size_t b = v.find_last_not_of(" \t");
                if (a != std::string::npos) key = v.substr(a, b - a + 1);
            }
        }
    }
    if (key.empty()) return false;
    const std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                             "Upgrade: websocket\r\n"
                             "Connection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: " +
                             accept_key(key) + "\r\n\r\n";
    return sock.send_all(resp.data(), resp.size());
}

/// Build a server→client **text** frame (FIN set, opcode 0x1, unmasked - servers
/// never mask). Handles the 7 / 16 / 64-bit payload-length encodings.
inline std::string frame_text(const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x81));  // FIN + text opcode
    const std::size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>(n));
    } else if (n <= 0xFFFF) {
        f.push_back(static_cast<char>(126));
        f.push_back(static_cast<char>((n >> 8) & 0xFF));
        f.push_back(static_cast<char>(n & 0xFF));
    } else {
        f.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i)
            f.push_back(static_cast<char>((static_cast<std::uint64_t>(n) >> (8 * i)) & 0xFF));
    }
    f += payload;
    return f;
}
inline bool send_text(mt5::LineSocket& sock, const std::string& payload) {
    const std::string f = frame_text(payload);
    return sock.send_all(f.data(), f.size());
}

/// Read one frame's payload (unmasks if the masked bit is set - clients must mask,
/// servers must not). `opcode`: 0x1 text, 0x8 close, 0x9 ping, 0xA pong. Returns
/// false on close/error.
inline bool read_frame(mt5::LineSocket& sock, std::string& payload, std::uint8_t& opcode) {
    char h[2];
    if (!sock.recv_exact(h, 2)) return false;
    opcode               = static_cast<std::uint8_t>(h[0] & 0x0F);
    const bool    masked = (static_cast<std::uint8_t>(h[1]) & 0x80) != 0;
    std::uint64_t len    = static_cast<std::uint8_t>(h[1]) & 0x7F;
    if (len == 126) {
        char e[2];
        if (!sock.recv_exact(e, 2)) return false;
        len = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(e[0])) << 8) |
              static_cast<std::uint8_t>(e[1]);
    } else if (len == 127) {
        char e[8];
        if (!sock.recv_exact(e, 8)) return false;
        len = 0;
        for (int i = 0; i < 8; ++i)
            len = (len << 8) | static_cast<std::uint8_t>(e[i]);
    }
    char mask[4] = {0, 0, 0, 0};
    if (masked && !sock.recv_exact(mask, 4)) return false;
    payload.assign(static_cast<std::size_t>(len), '\0');
    if (len && !sock.recv_exact(&payload[0], static_cast<std::size_t>(len))) return false;
    if (masked)
        for (std::uint64_t i = 0; i < len; ++i)
            payload[i] = static_cast<char>(payload[i] ^ mask[i & 3]);
    return true;
}

/// Client side (for the loopback test): send a minimal upgrade request with a
/// fixed key and verify the server's `Sec-WebSocket-Accept`. Returns true on a
/// successful handshake.
inline bool client_open(mt5::LineSocket& sock, const std::string& path = "/") {
    static const char kKey[] = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string req = "GET " + path + " HTTP/1.1\r\n"
                            "Host: localhost\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: " + std::string(kKey) + "\r\n"
                            "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!sock.send_all(req.data(), req.size())) return false;

    bool        ok = false;
    std::string line, accept;
    for (;;) {
        if (!sock.recv_line(line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if (line.find("101") != std::string::npos) ok = true;
        const std::string h = "sec-websocket-accept:";
        if (line.size() >= h.size()) {
            std::string lower = line.substr(0, h.size());
            for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (lower == h) {
                std::string v = line.substr(line.find(':') + 1);
                const std::size_t a = v.find_first_not_of(" \t");
                const std::size_t b = v.find_last_not_of(" \t");
                if (a != std::string::npos) accept = v.substr(a, b - a + 1);
            }
        }
    }
    return ok && accept == accept_key(kKey);
}

}  // namespace ws
}  // namespace hftob

#endif  // !_WIN32
