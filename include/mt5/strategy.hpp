#pragma once

#include <string>

#include "core/types.hpp"
#include "mt5/protocol.hpp"

namespace hftob {
namespace mt5 {

/// A tiny, deterministic example strategy that turns ticks into orders, purely to
/// exercise the bridge round trip end to end. Rule: once primed, if the mid price
/// jumps up by at least `step` versus the previous tick, emit a market **Buy**;
/// if it drops by at least `step`, emit a market **Sell**. It is intentionally
/// trivial — the point of this repo is the market-data engine and the bridge, not
/// alpha. Replace `on_tick` with real logic to drive live orders.
class ExampleStrategy {
public:
    explicit ExampleStrategy(double step = 0.0010, double lots = 0.10)
        : step_(step), lots_(lots) {}

    /// Returns true and fills `out` when the strategy decides to trade.
    bool on_tick(const Tick& t, Order& out) {
        const double mid = 0.5 * (t.bid + t.ask);
        bool fire = false;
        if (primed_) {
            if (mid - prev_mid_ >= step_) {
                out  = make(t.symbol, Side::Buy);
                fire = true;
            } else if (prev_mid_ - mid >= step_) {
                out  = make(t.symbol, Side::Sell);
                fire = true;
            }
        }
        prev_mid_ = mid;
        primed_   = true;
        return fire;
    }

private:
    Order make(const std::string& symbol, Side side) const {
        Order o;
        o.symbol = symbol;
        o.side   = side;
        o.volume = lots_;
        o.price  = 0.0;       // market
        o.kind   = "market";
        return o;
    }

    double prev_mid_ = 0.0;
    bool   primed_   = false;
    double step_;
    double lots_;
};

}  // namespace mt5
}  // namespace hftob
