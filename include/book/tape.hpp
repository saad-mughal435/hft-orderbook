#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/types.hpp"

namespace hftob {

/// One OHLCV bar over a fixed time bucket. Prices are integer ticks; `vwap` is in
/// dollars (volume-weighted average price within the bar).
struct Bar {
    std::uint64_t start_ns = 0;
    Price         open  = 0;
    Price         high  = 0;
    Price         low   = 0;
    Price         close = 0;
    std::uint64_t volume = 0;
    double        vwap   = 0.0;
};

/// The **trade tape**: consumes executed trades (ITCH `P` / `Q`) — distinct from
/// the order book, which tracks resting liquidity — and derives last price,
/// cumulative volume, running VWAP, and OHLCV bars bucketed by a fixed nanosecond
/// interval. The book reconstructs *intent*; the tape records what actually traded.
class Tape {
public:
    explicit Tape(std::uint64_t bar_interval_ns = 1000000000ull)
        : interval_(bar_interval_ns ? bar_interval_ns : 1) {}

    void on_trade(Price px, Qty qty, std::uint64_t ts) {
        has_last_ = true;
        last_px_  = px;
        last_ts_  = ts;
        cum_vol_      += qty;
        cum_notional_ += static_cast<long double>(px) * qty;

        const std::uint64_t bucket = ts / interval_;
        if (!cur_open_) {
            start_bar(px, bucket);
        } else if (bucket != cur_bucket_) {
            close_bar();
            start_bar(px, bucket);
        }
        if (px > cur_.high) cur_.high = px;
        if (px < cur_.low)  cur_.low  = px;
        cur_.close   = px;
        cur_.volume += qty;
        cur_notional_ += static_cast<long double>(px) * qty;
    }

    bool          has_last() const { return has_last_; }
    Price         last()     const { return last_px_; }
    std::uint64_t last_ts()  const { return last_ts_; }
    std::uint64_t volume()   const { return cum_vol_; }
    /// Cumulative VWAP in dollars.
    double vwap() const {
        return cum_vol_ ? static_cast<double>(cum_notional_ / cum_vol_) / kPriceScale : 0.0;
    }

    /// Completed bars; the in-progress bar is appended when `include_current`.
    std::vector<Bar> bars(bool include_current = true) const {
        std::vector<Bar> out = bars_;
        if (include_current && cur_open_) out.push_back(finalize(cur_, cur_notional_));
        return out;
    }
    std::size_t bar_count() const { return bars_.size() + (cur_open_ ? 1 : 0); }

private:
    void start_bar(Price px, std::uint64_t bucket) {
        cur_          = Bar{};
        cur_.start_ns = bucket * interval_;
        cur_.open = cur_.high = cur_.low = cur_.close = px;
        cur_bucket_   = bucket;
        cur_notional_ = 0.0L;
        cur_open_     = true;
    }
    void close_bar() {
        bars_.push_back(finalize(cur_, cur_notional_));
        cur_open_ = false;
    }
    static Bar finalize(Bar b, long double notional) {
        b.vwap = b.volume ? static_cast<double>(notional / b.volume) / kPriceScale : 0.0;
        return b;
    }

    std::uint64_t interval_;
    bool          has_last_     = false;
    Price         last_px_      = 0;
    std::uint64_t last_ts_      = 0;
    std::uint64_t cum_vol_      = 0;
    long double   cum_notional_ = 0.0L;

    std::vector<Bar> bars_;
    Bar              cur_{};
    bool             cur_open_     = false;
    std::uint64_t    cur_bucket_   = 0;
    long double      cur_notional_ = 0.0L;
};

}  // namespace hftob
