#pragma once

#include <cstddef>
#include <cstdint>

#include "core/types.hpp"

namespace hftob {

/// Order-book microstructure metrics derived from a reconstructed book. These are
/// the signals a market-data engine actually publishes: the micro-price, the
/// bid/ask spread, and order-book imbalance (relative resting size, a short-horizon
/// directional indicator). All prices are in dollars; sizes in shares.
struct BookMetrics {
    bool          has_bid     = false;
    bool          has_ask     = false;
    bool          two_sided   = false;
    Price         bid_px      = 0;     // ticks
    Price         ask_px      = 0;     // ticks
    Qty           bid_qty     = 0;     // top-of-book size
    Qty           ask_qty     = 0;
    double        mid         = 0.0;   // (bid + ask) / 2, dollars
    double        microprice  = 0.0;   // size-weighted fair value, dollars
    double        weighted_mid = 0.0;  // depth-weighted average price over N levels, dollars
    Price         spread_ticks = 0;
    double        spread_bps  = 0.0;   // spread / mid, in basis points
    double        imbalance_top = 0.0; // (bid_qty - ask_qty) / (bid_qty + ask_qty), [-1, 1]
    double        imbalance_n   = 0.0; // same over the top N levels of depth
    std::uint64_t bid_depth_n = 0;     // summed bid size over N levels
    std::uint64_t ask_depth_n = 0;
};

/// Compute metrics from any book exposing `best_bid` / `best_ask` and
/// `bids(n)` / `asks(n)` (i.e. `BasicOrderBook<...>`). Pure - no book state is
/// touched, so it is safe to call from a reader. The **micro-price** weights each
/// side's price by the *opposite* side's size, so heavy bids pull fair value
/// toward the ask (the classic short-term predictor). Empty / one-sided / crossed
/// books are handled (the relevant flags stay false and divisions are guarded).
template <typename Book>
BookMetrics compute_metrics(const Book& book, std::size_t n_levels = 5) {
    BookMetrics m;
    Price       bp = 0, ap = 0;
    Qty         bq = 0, aq = 0;
    m.has_bid = book.best_bid(bp, bq);
    m.has_ask = book.best_ask(ap, aq);
    if (m.has_bid) { m.bid_px = bp; m.bid_qty = bq; }
    if (m.has_ask) { m.ask_px = ap; m.ask_qty = aq; }
    if (!m.has_bid || !m.has_ask) return m;

    m.two_sided = true;
    const double bidd = to_dollars(bp);
    const double askd = to_dollars(ap);
    m.mid          = 0.5 * (bidd + askd);
    m.spread_ticks = ap - bp;
    m.spread_bps   = (m.mid > 0.0) ? (to_dollars(m.spread_ticks) / m.mid) * 1e4 : 0.0;

    const double top = static_cast<double>(bq) + static_cast<double>(aq);
    m.microprice    = (top > 0.0)
                          ? (bidd * static_cast<double>(aq) + askd * static_cast<double>(bq)) / top
                          : m.mid;
    m.imbalance_top = (top > 0.0)
                          ? (static_cast<double>(bq) - static_cast<double>(aq)) / top
                          : 0.0;

    // Depth-weighted average price + N-level imbalance.
    long double    num = 0.0L, den = 0.0L;
    std::uint64_t  bsum = 0, asum = 0;
    for (const auto& lv : book.bids(n_levels)) {
        num += static_cast<long double>(to_dollars(lv.first)) * lv.second;
        den += lv.second;
        bsum += lv.second;
    }
    for (const auto& lv : book.asks(n_levels)) {
        num += static_cast<long double>(to_dollars(lv.first)) * lv.second;
        den += lv.second;
        asum += lv.second;
    }
    m.bid_depth_n  = bsum;
    m.ask_depth_n  = asum;
    m.weighted_mid = (den > 0.0L) ? static_cast<double>(num / den) : m.mid;
    const double dn = static_cast<double>(bsum) + static_cast<double>(asum);
    m.imbalance_n  = (dn > 0.0) ? (static_cast<double>(bsum) - static_cast<double>(asum)) / dn : 0.0;
    return m;
}

}  // namespace hftob
