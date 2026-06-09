#pragma once

#include <cstdint>

namespace hftob {

/// ITCH prices are unsigned 4-byte integers with 4 implied decimal places, i.e.
/// the integer is the price in units of 1/10000 of a dollar. We keep them as a
/// signed 64-bit integer of those ticks and only convert to a double for display.
using Price    = std::int64_t;
using Qty      = std::uint32_t;
using OrderRef = std::uint64_t;

enum class Side : char { Buy = 'B', Sell = 'S', None = '\0' };

/// One price tick == $0.0001.
constexpr Price kPriceScale = 10000;

constexpr double to_dollars(Price ticks) {
    return static_cast<double>(ticks) / static_cast<double>(kPriceScale);
}

}  // namespace hftob
