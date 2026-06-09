// WebSocket codec unit tests + a real loopback handshake/frame test. POSIX only
// (the WS layer is built on the POSIX line-socket), so this is its own UNIX target.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <thread>

#include "feed/websocket.hpp"
#include "mt5/tcp.hpp"

using namespace hftob;
using namespace hftob::mt5;

TEST_CASE("ws frame_text encodes the 7- and 16-bit length forms", "[ws]") {
    const std::string f1 = ws::frame_text("hello");
    REQUIRE(f1.size() == 2 + 5);
    CHECK(static_cast<std::uint8_t>(f1[0]) == 0x81);  // FIN + text
    CHECK(static_cast<std::uint8_t>(f1[1]) == 5);
    CHECK(f1.substr(2) == "hello");

    const std::string big(200, 'x');
    const std::string f2 = ws::frame_text(big);
    REQUIRE(f2.size() == 4 + 200);
    CHECK(static_cast<std::uint8_t>(f2[0]) == 0x81);
    CHECK(static_cast<std::uint8_t>(f2[1]) == 126);   // 16-bit length marker
    const int len = (static_cast<std::uint8_t>(f2[2]) << 8) | static_cast<std::uint8_t>(f2[3]);
    CHECK(len == 200);
}

TEST_CASE("ws accept-key matches the RFC 6455 example", "[ws]") {
    CHECK(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("ws loopback: handshake then server text frames are received by the client",
          "[ws][itest][thread]") {
    Listener            listener(0);
    const std::uint16_t port = listener.port();
    constexpr int       N    = 5;

    bool server_hs = false;
    std::thread server([&] {
        LineSocket conn = listener.accept();
        server_hs = ws::handshake(conn);
        if (server_hs)
            for (int i = 0; i < N; ++i)
                ws::send_text(conn, "{\"symbol\":\"SYNTH\",\"seq\":" + std::to_string(i) +
                                        ",\"bids\":[[100.0,5]],\"asks\":[[100.01,3]]}");
    });

    LineSocket client  = LineSocket::connect("127.0.0.1", port);
    const bool opened  = ws::client_open(client);
    int        got     = 0;
    bool       all_ok  = true;
    if (opened) {
        for (int i = 0; i < N; ++i) {
            std::string  payload;
            std::uint8_t op = 0;
            if (!ws::read_frame(client, payload, op)) { all_ok = false; break; }
            if (op != 0x1 || payload.find("\"symbol\"") == std::string::npos ||
                payload.find("\"bids\"") == std::string::npos)
                all_ok = false;
            ++got;
        }
    }
    server.join();

    CHECK(opened);
    CHECK(server_hs);
    CHECK(got == N);
    CHECK(all_ok);
}
