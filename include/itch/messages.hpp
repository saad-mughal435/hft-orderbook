#pragma once

#include <cstdint>

#include "core/types.hpp"

namespace hftob {
namespace itch {

/// The NASDAQ TotalView-ITCH 5.0 message types this engine reconstructs a book
/// from. Tape-only messages (P/Q) and reference data (R/S) are decoded too so a
/// full feed can be replayed; everything else is skipped by length.
enum class MsgType : char {
    Unknown                = '\0',
    SystemEvent            = 'S',
    StockDirectory         = 'R',
    AddOrder               = 'A',
    AddOrderMpid           = 'F',
    OrderExecuted          = 'E',
    OrderExecutedWithPrice = 'C',
    OrderCancel            = 'X',
    OrderDelete            = 'D',
    OrderReplace           = 'U',
    Trade                  = 'P',
    CrossTrade             = 'Q',
};

/// A decoded ITCH message in a single fixed-size, poolable struct. Only the
/// fields meaningful for a given `type` are populated; the rest stay zeroed.
/// ITCH is order-based (every order carries an explicit reference number and the
/// exchange has already matched), so this drives a reconstructor, not a matcher.
struct Message {
    MsgType       type           = MsgType::Unknown;
    std::uint16_t stock_locate   = 0;   // per-day instrument index (header)
    std::uint64_t timestamp      = 0;   // nanoseconds since midnight
    std::uint64_t order_ref      = 0;   // Add/Exec/Cancel/Delete; Replace=original
    std::uint64_t new_order_ref  = 0;   // OrderReplace only
    std::uint64_t match_number   = 0;   // Exec/Trade
    Side          side           = Side::None;  // AddOrder / Trade
    std::uint32_t shares         = 0;   // add | executed | cancelled | replace shares
    Price         price          = 0;   // add | replace | exec-with-price
    char          stock[8]       = {};  // Add/Trade/StockDirectory (space-padded)
    char          event_code     = 0;   // SystemEvent
};

}  // namespace itch
}  // namespace hftob
