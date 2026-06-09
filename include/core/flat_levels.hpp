#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <utility>
#include <vector>

#include "core/types.hpp"

namespace hftob {

/// Price-level store backed by `std::map` (a red-black tree). `Compare` orders the
/// levels so the best quote is first - `std::greater` for bids (highest first),
/// `std::less` for asks (lowest first). This is the readable baseline that the
/// cache-friendly `FlatLevels` is benchmarked against. Both expose the same
/// interface so `BasicOrderBook` is templated over the choice.
template <typename Compare>
class MapLevels {
public:
    void inc(Price p, Qty q) { m_[p] += q; }
    void dec(Price p, Qty q) {
        auto it = m_.find(p);
        if (it != m_.end() && (it->second -= q) == 0) m_.erase(it);
    }
    bool best(Price& p, Qty& q) const {
        if (m_.empty()) return false;
        p = m_.begin()->first;
        q = m_.begin()->second;
        return true;
    }
    Qty at(Price p) const {
        auto it = m_.find(p);
        return it == m_.end() ? Qty{0} : it->second;
    }
    std::size_t size() const { return m_.size(); }
    std::vector<std::pair<Price, Qty>> top(std::size_t n) const {
        std::vector<std::pair<Price, Qty>> out;
        out.reserve(n);
        for (const auto& kv : m_) {
            if (out.size() >= n) break;
            out.emplace_back(kv.first, kv.second);
        }
        return out;
    }

private:
    std::map<Price, Qty, Compare> m_;
};

/// Price-level store backed by a **sorted `std::vector`**. Levels are kept ordered
/// by `Compare(price)` so the best quote is at the front (O(1) best, contiguous
/// and cache-friendly) and lookups are binary search. Inserts/erases shift the
/// tail, but order-book activity clusters at the inside of the book, so the shifts
/// are short in practice. The cache-friendly counterpart to `MapLevels`.
template <typename Compare>
class FlatLevels {
public:
    void inc(Price p, Qty q) {
        auto it = find(p);
        if (it != v_.end() && it->first == p) it->second += q;
        else                                  v_.insert(it, {p, q});
    }
    void dec(Price p, Qty q) {
        auto it = find(p);
        if (it != v_.end() && it->first == p && (it->second -= q) == 0) v_.erase(it);
    }
    bool best(Price& p, Qty& q) const {
        if (v_.empty()) return false;
        p = v_.front().first;
        q = v_.front().second;
        return true;
    }
    Qty at(Price p) const {
        auto it = find(p);
        return (it != v_.end() && it->first == p) ? it->second : Qty{0};
    }
    std::size_t size() const { return v_.size(); }
    std::vector<std::pair<Price, Qty>> top(std::size_t n) const {
        std::vector<std::pair<Price, Qty>> out;
        out.reserve(n);
        for (std::size_t i = 0; i < v_.size() && i < n; ++i) out.push_back(v_[i]);
        return out;
    }

private:
    // First element whose price is not "better than" p, per Compare - i.e. the
    // position of p if present, else its insertion point that preserves order.
    // (auto-deduced iterator type avoids naming the dependent member iterator.)
    auto find(Price p) {
        return std::lower_bound(v_.begin(), v_.end(), p,
            [this](const std::pair<Price, Qty>& a, Price key) { return cmp_(a.first, key); });
    }
    auto find(Price p) const {
        return std::lower_bound(v_.begin(), v_.end(), p,
            [this](const std::pair<Price, Qty>& a, Price key) { return cmp_(a.first, key); });
    }

    std::vector<std::pair<Price, Qty>> v_;
    Compare                            cmp_{};
};

}  // namespace hftob
