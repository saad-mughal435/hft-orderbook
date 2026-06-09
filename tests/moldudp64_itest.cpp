// MoldUDP64 over a real UDP loopback socket: packetize a synthetic ITCH stream,
// send each datagram, receive it, and reconstruct the book through MoldSession.
// Send/recv are interleaved 1:1 so the kernel UDP buffer never overflows (no
// spurious drops). POSIX only.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "book/book_set.hpp"
#include "feed/framing.hpp"
#include "feed/moldudp64.hpp"
#include "feed/synthetic.hpp"
#include "feed/udp.hpp"
#include "itch/messages.hpp"

using namespace hftob;

TEST_CASE("MoldUDP64 over a real UDP loopback socket reconstructs the book",
          "[mold][itest][udp]") {
    const std::vector<std::uint8_t> framed = make_synthetic_itch(1500, 11);
    const auto packets = packetize_mold("SESSION01", framed.data(), framed.size(), 8);
    REQUIRE(!packets.empty());

    UdpSocket           rx;
    const std::uint16_t port = rx.bind_loopback(0);
    rx.set_recv_timeout(2000);
    UdpSocket tx;

    MoldSession  sess;
    BookSet      book;
    std::uint8_t buf[2048];
    for (const auto& p : packets) {
        REQUIRE(tx.send_to(p.data(), p.size(), port));
        const ssize_t got = rx.recv(buf, sizeof(buf));
        REQUIRE(got > 0);
        sess.on_packet(buf, static_cast<std::size_t>(got),
                       [&](std::uint64_t, const itch::Message& m) { book.apply(m); });
    }

    BookSet     ref;
    std::size_t framed_count = 0;
    for_each_framed_message(framed.data(), framed.size(), [&](const itch::Message& m) {
        ref.apply(m);
        ++framed_count;
    });

    CHECK(sess.messages() == framed_count);
    CHECK(sess.gaps() == 0);
    CHECK(book.total_orders() == ref.total_orders());
    CHECK(book.book_count() == ref.book_count());
}
