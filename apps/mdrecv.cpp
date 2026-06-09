// mdrecv — MoldUDP64 market-data receiver. Reconstructs books from the *real-time*
// UDP feed framing (BinaryFILE is for the historical files; MoldUDP64 wraps the
// same ITCH messages in sequenced UDP datagrams for live multicast).
//
//   mdrecv --self [N]               generate N synthetic msgs, packetize + parse
//                                   locally (no socket) — demo / CI smoke
//   mdrecv --listen <port> [--ms T] bind 127.0.0.1:<port>, receive datagrams until
//                                   end-of-session or T ms idle (default 2000)

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "book/book_set.hpp"
#include "feed/moldudp64.hpp"
#include "feed/synthetic.hpp"
#include "feed/udp.hpp"
#include "itch/messages.hpp"

using namespace hftob;

namespace {
void print_stats(const MoldSession& s, const BookSet& b) {
    std::cout << "  packets=" << s.packets() << " messages=" << s.messages()
              << " gaps=" << s.gaps() << " books=" << b.book_count()
              << " orders=" << b.total_orders() << "\n";
}
}  // namespace

int main(int argc, char** argv) {
    bool self = false, listen = false;
    long n = 20000, port = 0, ms = 2000;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self") {
            self = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') n = std::strtol(argv[++i], nullptr, 10);
        } else if (a == "--listen" && i + 1 < argc) {
            listen = true;
            port   = std::strtol(argv[++i], nullptr, 10);
        } else if (a == "--ms" && i + 1 < argc) {
            ms = std::strtol(argv[++i], nullptr, 10);
        }
    }
    if (!self && !listen) self = true;

    if (self) {
        const std::vector<std::uint8_t> framed =
            make_synthetic_itch(static_cast<std::size_t>(n > 0 ? n : 1), 7);
        const auto  packets = packetize_mold("HFTOB-SYN", framed.data(), framed.size(), 8);
        MoldSession s;
        BookSet     book;
        for (const auto& p : packets)
            s.on_packet(p.data(), p.size(),
                        [&](std::uint64_t, const itch::Message& m) { book.apply(m); });
        std::cout << "mdrecv --self " << n << " (MoldUDP64 packetize + parse, no socket)\n";
        print_stats(s, book);
        return 0;
    }

    try {
        UdpSocket           rx;
        const std::uint16_t bound = rx.bind_loopback(static_cast<std::uint16_t>(port));
        rx.set_recv_timeout(static_cast<int>(ms));
        std::cerr << "mdrecv listening on UDP 127.0.0.1:" << bound << " (idle " << ms << " ms)\n";

        MoldSession  s;
        BookSet      book;
        std::uint8_t buf[2048];
        for (;;) {
            const ssize_t got = rx.recv(buf, sizeof(buf));
            if (got <= 0) break;  // idle timeout
            s.on_packet(buf, static_cast<std::size_t>(got),
                        [&](std::uint64_t, const itch::Message& m) { book.apply(m); });
            if (s.ended()) break;
        }
        std::cout << "mdrecv done\n";
        print_stats(s, book);
    } catch (const std::exception& e) {
        std::cerr << "mdrecv error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
