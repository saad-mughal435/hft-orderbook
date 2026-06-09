#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "book/book_set.hpp"
#include "feed/framing.hpp"
#include "feed/moldudp64.hpp"
#include "feed/synthetic.hpp"
#include "itch/messages.hpp"

using namespace hftob;

TEST_CASE("MoldUDP64 packetize + parse reconstructs the same book", "[mold]") {
    const std::vector<std::uint8_t> framed = make_synthetic_itch(2000, 5);

    std::size_t framed_count = 0;
    BookSet     ref;
    for_each_framed_message(framed.data(), framed.size(), [&](const itch::Message& m) {
        ref.apply(m);
        ++framed_count;
    });

    const auto packets = packetize_mold("SESSION01", framed.data(), framed.size(), 8);
    REQUIRE(!packets.empty());

    MoldSession sess;
    BookSet     via_mold;
    for (const auto& p : packets)
        sess.on_packet(p.data(), p.size(),
                       [&](std::uint64_t, const itch::Message& m) { via_mold.apply(m); });

    CHECK(sess.messages() == framed_count);
    CHECK(sess.gaps() == 0);
    CHECK(sess.expected_seq() == framed_count + 1);   // sequence numbers start at 1
    CHECK(via_mold.book_count() == ref.book_count());
    CHECK(via_mold.total_orders() == ref.total_orders());
}

TEST_CASE("MoldSession detects a dropped datagram as a sequence gap", "[mold]") {
    const std::vector<std::uint8_t> framed = make_synthetic_itch(800, 9);
    const auto packets = packetize_mold("SESSION01", framed.data(), framed.size(), 4);
    REQUIRE(packets.size() >= 3);

    MoldSession sess;
    std::size_t delivered = 0;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        if (i == 1) continue;   // "drop" the second datagram on the wire
        sess.on_packet(packets[i].data(), packets[i].size(),
                       [&](std::uint64_t, const itch::Message&) { ++delivered; });
    }
    CHECK(sess.gaps() > 0);                       // the drop is detected as missing seqs
    CHECK(delivered < sess.expected_seq() - 1);
}

TEST_CASE("MoldUDP64 end-of-session marker is recognised", "[mold]") {
    const auto eos = encode_mold_packet("SESSION01", 100, kMoldEndOfSession, nullptr, 0);
    MoldSession sess;
    sess.on_packet(eos.data(), eos.size(), [&](std::uint64_t, const itch::Message&) {});
    CHECK(sess.ended());
    CHECK(sess.messages() == 0);
}
