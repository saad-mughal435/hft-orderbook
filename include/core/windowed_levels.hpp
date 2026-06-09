#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "core/types.hpp"

namespace hftob {

/// Price-level store backed by a **price-tick-indexed array windowed around the
/// inside** — the canonical L2 structure in low-latency books. A contiguous
/// `slots_` array maps a window of `W` consecutive ticks `[base_, base_+W)` to
/// quantities (O(1) `inc`/`dec`/`at` by direct indexing), with a tracked
/// `best_idx_` for O(1) `best`. Levels far outside the window spill to a small
/// `std::map` overflow; when the inside moves out of the window the store
/// **re-centres** (rare). `Compare` (`std::greater` for bids / `std::less` for
/// asks) selects which side is "better". Same interface as `MapLevels` /
/// `FlatLevels`, so it drops into `BasicOrderBook<>` and the benchmark A/B.
template <typename Compare>
class WindowedLevels {
public:
    void inc(Price p, Qty q) {
        if (q == 0) return;
        if (!has_base_) {
            recenter_to(p);
        } else if (!in_window(p)) {
            if ((count_ == 0 && overflow_.empty()) || better(p, best_price()))
                recenter_to(p);                 // new inside lands beyond the window
            else {
                overflow_[p] += q;              // far on the worse side: spill
                return;
            }
        }
        place_in_window(p, q);
    }

    void dec(Price p, Qty q) {
        if (in_window(p)) {
            const std::size_t idx = index(p);
            if (slots_[idx] == 0) return;
            if ((slots_[idx] -= q) == 0) {
                --count_;
                if (idx == best_idx_) rescan_best();
            }
        } else {
            auto it = overflow_.find(p);
            if (it != overflow_.end() && (it->second -= q) == 0) overflow_.erase(it);
        }
    }

    bool best(Price& price, Qty& qty) const {
        const bool hw = (count_ > 0);
        const bool ho = !overflow_.empty();
        if (!hw && !ho) return false;
        // The best is the better of the window's best and the overflow's best.
        if (hw && (!ho || !better(overflow_.begin()->first, window_best_price()))) {
            price = window_best_price();
            qty   = slots_[best_idx_];
        } else {
            price = overflow_.begin()->first;
            qty   = overflow_.begin()->second;
        }
        return true;
    }

    Qty at(Price p) const {
        if (in_window(p)) return slots_[index(p)];
        auto it = overflow_.find(p);
        return it == overflow_.end() ? Qty{0} : it->second;
    }

    std::size_t size() const { return count_ + overflow_.size(); }

    /// Top-`n` levels, best first — a merge of the window (walked from the inside
    /// outward in `Compare` order) and the overflow map (already in `Compare` order).
    std::vector<std::pair<Price, Qty>> top(std::size_t n) const {
        std::vector<std::pair<Price, Qty>> out;
        if (n == 0) return out;
        out.reserve(n);

        std::size_t wi      = best_idx_;
        bool        w_valid = (count_ > 0);
        auto        of      = overflow_.begin();

        while (out.size() < n && (w_valid || of != overflow_.end())) {
            const bool take_window =
                w_valid && (of == overflow_.end() ||
                            !better(of->first, base_ + static_cast<Price>(wi)));
            if (take_window) {
                out.emplace_back(base_ + static_cast<Price>(wi), slots_[wi]);
                w_valid = next_window_idx(wi);
            } else {
                out.emplace_back(of->first, of->second);
                ++of;
            }
        }
        return out;
    }

private:
    static constexpr std::size_t kWindow = 1u << 16;  // 65536 ticks (~$6.55) per side

    bool        better(Price a, Price b) const { return cmp_(a, b); }
    bool        in_window(Price p) const {
        return has_base_ && p >= base_ && static_cast<Price>(p - base_) < static_cast<Price>(kWindow);
    }
    std::size_t index(Price p) const { return static_cast<std::size_t>(p - base_); }
    Price       window_best_price() const { return base_ + static_cast<Price>(best_idx_); }
    Price       best_price() const {  // best across window + overflow (window assumed non-empty here)
        if (count_ == 0) return overflow_.begin()->first;
        if (overflow_.empty() || !better(overflow_.begin()->first, window_best_price()))
            return window_best_price();
        return overflow_.begin()->first;
    }

    // Walk `i` to the next non-empty window slot in Compare order; false if none.
    bool next_window_idx(std::size_t& i) const {
        const bool high_first = cmp_(Price{1}, Price{0});  // bids: higher index first
        for (;;) {
            if (high_first) {
                if (i == 0) return false;
                --i;
            } else {
                ++i;
                if (i >= slots_.size()) return false;
            }
            if (slots_[i] != 0) return true;
        }
    }

    void place_in_window(Price p, Qty q) {
        const std::size_t idx = index(p);
        const bool        was_empty = (slots_[idx] == 0);
        slots_[idx] += q;
        if (was_empty) {
            if (count_ == 0 || better(p, window_best_price())) best_idx_ = idx;
            ++count_;
        }
    }

    void rescan_best() {
        if (count_ == 0) { best_idx_ = 0; return; }
        std::size_t i = best_idx_;
        if (next_window_idx(i)) { best_idx_ = i; return; }  // next-best is "worse" of old best
        // Safety net (shouldn't trigger if best_idx_ was truly the best): full scan.
        const bool high_first = cmp_(Price{1}, Price{0});
        if (high_first) {
            for (std::size_t j = slots_.size(); j-- > 0;)
                if (slots_[j] != 0) { best_idx_ = j; return; }
        } else {
            for (std::size_t j = 0; j < slots_.size(); ++j)
                if (slots_[j] != 0) { best_idx_ = j; return; }
        }
    }

    // Collect every level, re-base the window on `center`, and re-bucket. O(W +
    // overflow); only on first insert or when the inside leaves the window.
    void recenter_to(Price center) {
        std::vector<std::pair<Price, Qty>> all;
        if (has_base_) {
            for (std::size_t i = 0; i < slots_.size(); ++i)
                if (slots_[i] != 0) all.emplace_back(base_ + static_cast<Price>(i), slots_[i]);
        }
        for (const auto& kv : overflow_) all.emplace_back(kv.first, kv.second);

        Price new_base = center - static_cast<Price>(kWindow / 2);
        if (new_base < 0) new_base = 0;
        base_     = new_base;
        has_base_ = true;
        slots_.assign(kWindow, 0);
        count_    = 0;
        best_idx_ = 0;
        overflow_.clear();
        for (const auto& kv : all) {
            if (in_window(kv.first)) place_in_window(kv.first, kv.second);
            else                     overflow_[kv.first] += kv.second;
        }
    }

    std::vector<Qty>              slots_;        // qty at price base_+i (0 = empty)
    std::map<Price, Qty, Compare> overflow_;     // far-out levels (rare)
    Price                         base_      = 0;
    std::size_t                   best_idx_  = 0; // index of the best in-window level
    std::size_t                   count_     = 0; // non-empty window slots
    bool                          has_base_  = false;
    Compare                       cmp_{};
};

}  // namespace hftob
