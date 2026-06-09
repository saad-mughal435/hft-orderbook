#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>

#include "core/sha1.hpp"

using namespace hftob;

namespace {
std::string hex(const std::array<std::uint8_t, 20>& d) {
    static const char H[] = "0123456789abcdef";
    std::string       s;
    for (const auto b : d) {
        s.push_back(H[b >> 4]);
        s.push_back(H[b & 15]);
    }
    return s;
}
}  // namespace

TEST_CASE("sha1 matches known digests", "[sha1]") {
    CHECK(hex(sha1(std::string("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(sha1(std::string(""))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex(sha1(std::string("The quick brown fox jumps over the lazy dog"))) ==
          "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

TEST_CASE("base64 matches RFC 4648 vectors", "[sha1][base64]") {
    CHECK(base64(std::string("")) == "");
    CHECK(base64(std::string("f")) == "Zg==");
    CHECK(base64(std::string("fo")) == "Zm8=");
    CHECK(base64(std::string("foo")) == "Zm9v");
    CHECK(base64(std::string("foob")) == "Zm9vYg==");
    CHECK(base64(std::string("fooba")) == "Zm9vYmE=");
    CHECK(base64(std::string("foobar")) == "Zm9vYmFy");
}

TEST_CASE("RFC 6455 WebSocket accept-key computation", "[sha1][websocket]") {
    const std::string key   = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const auto        d     = sha1(key + magic);
    CHECK(base64(d.data(), d.size()) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}
