#include "itch/decoder.hpp"

#include <cstring>

namespace hftob {
namespace itch {
namespace {

// Big-endian (network order) field readers. Portable: no reliance on host
// endianness, alignment, or struct packing.
inline std::uint16_t rd16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((std::uint16_t(p[0]) << 8) | p[1]);
}
inline std::uint32_t rd32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}
inline std::uint64_t rd_be(const std::uint8_t* p, int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}
inline std::uint64_t rd48(const std::uint8_t* p) { return rd_be(p, 6); }
inline std::uint64_t rd64(const std::uint8_t* p) { return rd_be(p, 8); }

}  // namespace

std::size_t message_length(char type) {
    switch (type) {
        case 'S': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': return 20;
        case 'L': return 26;
        case 'V': return 35;
        case 'W': return 12;
        case 'K': return 28;
        case 'J': return 35;
        case 'h': return 21;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        case 'B': return 19;
        case 'I': return 50;
        case 'N': return 20;
        case 'O': return 48;
        default:  return 0;
    }
}

bool decode(const std::uint8_t* buf, std::size_t len, Message& out) {
    if (buf == nullptr || len < 11) return false;

    out = Message{};
    out.stock_locate = rd16(buf + 1);
    out.timestamp    = rd48(buf + 5);

    const char type = static_cast<char>(buf[0]);
    switch (type) {
        case 'S':
            if (len < 12) return false;
            out.type = MsgType::SystemEvent;
            out.event_code = static_cast<char>(buf[11]);
            return true;

        case 'R':
            if (len < 39) return false;
            out.type = MsgType::StockDirectory;
            std::memcpy(out.stock, buf + 11, 8);
            return true;

        case 'A':
            if (len < 36) return false;
            out.type = MsgType::AddOrder;
            out.order_ref = rd64(buf + 11);
            out.side = static_cast<Side>(buf[19]);
            out.shares = rd32(buf + 20);
            std::memcpy(out.stock, buf + 24, 8);
            out.price = static_cast<Price>(rd32(buf + 32));
            return true;

        case 'F':
            if (len < 40) return false;
            out.type = MsgType::AddOrderMpid;
            out.order_ref = rd64(buf + 11);
            out.side = static_cast<Side>(buf[19]);
            out.shares = rd32(buf + 20);
            std::memcpy(out.stock, buf + 24, 8);
            out.price = static_cast<Price>(rd32(buf + 32));
            return true;

        case 'E':
            if (len < 31) return false;
            out.type = MsgType::OrderExecuted;
            out.order_ref = rd64(buf + 11);
            out.shares = rd32(buf + 19);       // executed shares
            out.match_number = rd64(buf + 23);
            return true;

        case 'C':
            if (len < 36) return false;
            out.type = MsgType::OrderExecutedWithPrice;
            out.order_ref = rd64(buf + 11);
            out.shares = rd32(buf + 19);       // executed shares
            out.match_number = rd64(buf + 23);
            out.price = static_cast<Price>(rd32(buf + 32));  // print price
            return true;

        case 'X':
            if (len < 23) return false;
            out.type = MsgType::OrderCancel;
            out.order_ref = rd64(buf + 11);
            out.shares = rd32(buf + 19);       // cancelled shares
            return true;

        case 'D':
            if (len < 19) return false;
            out.type = MsgType::OrderDelete;
            out.order_ref = rd64(buf + 11);
            return true;

        case 'U':
            if (len < 35) return false;
            out.type = MsgType::OrderReplace;
            out.order_ref = rd64(buf + 11);      // original
            out.new_order_ref = rd64(buf + 19);  // new
            out.shares = rd32(buf + 27);
            out.price = static_cast<Price>(rd32(buf + 31));
            return true;

        case 'P':
            if (len < 44) return false;
            out.type = MsgType::Trade;
            out.order_ref = rd64(buf + 11);
            out.side = static_cast<Side>(buf[19]);
            out.shares = rd32(buf + 20);
            std::memcpy(out.stock, buf + 24, 8);
            out.price = static_cast<Price>(rd32(buf + 32));
            out.match_number = rd64(buf + 36);
            return true;

        case 'Q':
            if (len < 40) return false;
            out.type = MsgType::CrossTrade;
            std::memcpy(out.stock, buf + 19, 8);
            out.price = static_cast<Price>(rd32(buf + 27));
            out.match_number = rd64(buf + 31);
            return true;

        default:
            return false;  // recognised-but-unhandled or unknown type
    }
}

}  // namespace itch
}  // namespace hftob
