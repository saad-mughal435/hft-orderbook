#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hftob {

/// Compact SHA-1 (RFC 3174) + base64 (RFC 4648). Used only for the WebSocket
/// handshake (`Sec-WebSocket-Accept = base64(sha1(key + GUID))`), so the WS layer
/// needs no external crypto dependency. Not for security use.
inline std::array<std::uint8_t, 20> sha1(const std::uint8_t* data, std::size_t len) {
    std::uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476,
                  h4 = 0xC3D2E1F0;

    const std::uint64_t ml    = static_cast<std::uint64_t>(len) * 8;
    std::size_t         total = len + 1;
    while (total % 64 != 56) ++total;
    total += 8;
    std::vector<std::uint8_t> msg(total, 0);
    for (std::size_t i = 0; i < len; ++i) msg[i] = data[i];
    msg[len] = 0x80;
    for (int i = 0; i < 8; ++i)
        msg[total - 1 - static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((ml >> (8 * i)) & 0xFF);

    auto rol = [](std::uint32_t x, int c) {
        return static_cast<std::uint32_t>((x << c) | (x >> (32 - c)));
    };

    for (std::size_t chunk = 0; chunk < total; chunk += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const std::size_t o = chunk + static_cast<std::size_t>(4 * i);
            w[i] = (static_cast<std::uint32_t>(msg[o]) << 24) |
                   (static_cast<std::uint32_t>(msg[o + 1]) << 16) |
                   (static_cast<std::uint32_t>(msg[o + 2]) << 8) |
                   static_cast<std::uint32_t>(msg[o + 3]);
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
            const std::uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    const std::uint32_t            hs[5] = {h0, h1, h2, h3, h4};
    std::array<std::uint8_t, 20>   out{};
    for (int i = 0; i < 5; ++i) {
        out[static_cast<std::size_t>(4 * i)]     = static_cast<std::uint8_t>((hs[i] >> 24) & 0xFF);
        out[static_cast<std::size_t>(4 * i + 1)] = static_cast<std::uint8_t>((hs[i] >> 16) & 0xFF);
        out[static_cast<std::size_t>(4 * i + 2)] = static_cast<std::uint8_t>((hs[i] >> 8) & 0xFF);
        out[static_cast<std::size_t>(4 * i + 3)] = static_cast<std::uint8_t>(hs[i] & 0xFF);
    }
    return out;
}
inline std::array<std::uint8_t, 20> sha1(const std::string& s) {
    return sha1(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

inline std::string base64(const std::uint8_t* data, std::size_t len) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);
        out.push_back(T[n & 63]);
    }
    const std::size_t rem = len - i;
    if (rem == 1) {
        const std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}
inline std::string base64(const std::string& s) {
    return base64(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

}  // namespace hftob
