#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/types.hpp"
#include "itch/messages.hpp"

namespace hftob {

/// A single-instrument limit-order-book reconstructor.
///
/// ITCH is order-based, so the hot path is an O(1) `order_ref -> Order` hash map:
/// every Execute / Cancel / Delete / Replace looks the order up directly and
/// adjusts its resting price level. Price levels live in ordered maps so the best
/// bid/ask is simply the first element (bids descending, asks ascending), and the
/// total resting quantity per price is maintained incrementally.
class OrderBook {
public:
    struct Order {
        Side  side   = Side::None;
        Price price  = 0;
        Qty   shares = 0;
    };

    /// Add a resting order (ITCH Add / Add-with-MPID).
    void add(OrderRef ref, Side side, Price price, Qty shares) {
        if (shares == 0) return;
        orders_[ref] = Order{side, price, shares};
        if (side == Side::Buy) bids_[price] += shares;
        else                   asks_[price] += shares;
    }

    /// Reduce an order by `qty` (ITCH Order Executed / Order Cancel — partial).
    /// Removes the order if fully consumed. Returns false if the ref is unknown.
    bool reduce(OrderRef ref, Qty qty) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return false;
        Order& o = it->second;
        const Qty d = (qty < o.shares) ? qty : o.shares;
        o.shares -= d;
        decrement_level(o.side, o.price, d);
        if (o.shares == 0) orders_.erase(it);
        return true;
    }

    /// Remove an order entirely (ITCH Order Delete). Returns false if unknown.
    bool remove(OrderRef ref) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return false;
        const Order o = it->second;
        decrement_level(o.side, o.price, o.shares);
        orders_.erase(it);
        return true;
    }

    /// Replace an order (ITCH Order Replace): delete the original ref, add the
    /// new ref at a new price/size inheriting the original's side. Loses time
    /// priority, exactly as the exchange models it.
    bool replace(OrderRef orig_ref, OrderRef new_ref, Qty new_shares, Price new_price) {
        auto it = orders_.find(orig_ref);
        if (it == orders_.end()) return false;
        const Side side = it->second.side;  // read side before erasing
        remove(orig_ref);
        add(new_ref, side, new_price, new_shares);
        return true;
    }

    /// Apply a decoded ITCH message (order-book-affecting types only).
    void apply(const itch::Message& m) {
        using T = itch::MsgType;
        switch (m.type) {
            case T::AddOrder:
            case T::AddOrderMpid:
                add(m.order_ref, m.side, m.price, m.shares);
                break;
            case T::OrderExecuted:
            case T::OrderExecutedWithPrice:
            case T::OrderCancel:
                reduce(m.order_ref, m.shares);
                break;
            case T::OrderDelete:
                remove(m.order_ref);
                break;
            case T::OrderReplace:
                replace(m.order_ref, m.new_order_ref, m.shares, m.price);
                break;
            default:
                break;  // Trade / CrossTrade / SystemEvent / StockDirectory: tape-only
        }
    }

    // ---- queries ----------------------------------------------------------
    bool best_bid(Price& price, Qty& qty) const {
        if (bids_.empty()) return false;
        price = bids_.begin()->first;
        qty   = bids_.begin()->second;
        return true;
    }
    bool best_ask(Price& price, Qty& qty) const {
        if (asks_.empty()) return false;
        price = asks_.begin()->first;
        qty   = asks_.begin()->second;
        return true;
    }
    Qty qty_at(Side side, Price price) const {
        if (side == Side::Buy) {
            auto it = bids_.find(price);
            return (it == bids_.end()) ? 0u : it->second;
        }
        auto it = asks_.find(price);
        return (it == asks_.end()) ? 0u : it->second;
    }
    std::size_t order_count() const { return orders_.size(); }
    std::size_t bid_levels()  const { return bids_.size(); }
    std::size_t ask_levels()  const { return asks_.size(); }

    /// Top-`n` price levels on a side, best first (bids descending, asks
    /// ascending) — the inside of the book outward. Returns fewer than `n` if the
    /// side is thinner. The maps are already ordered, so this is a simple walk.
    std::vector<std::pair<Price, Qty>> depth(Side side, std::size_t n) const {
        std::vector<std::pair<Price, Qty>> out;
        out.reserve(n);
        if (side == Side::Buy) {
            for (const auto& kv : bids_) {
                if (out.size() >= n) break;
                out.emplace_back(kv.first, kv.second);
            }
        } else {
            for (const auto& kv : asks_) {
                if (out.size() >= n) break;
                out.emplace_back(kv.first, kv.second);
            }
        }
        return out;
    }
    std::vector<std::pair<Price, Qty>> bids(std::size_t n) const { return depth(Side::Buy, n); }
    std::vector<std::pair<Price, Qty>> asks(std::size_t n) const { return depth(Side::Sell, n); }

    /// Recompute every price level's quantity from the order map and check it
    /// matches the incrementally-maintained level totals — the core consistency
    /// invariant of the reconstructor. Also verifies each resting order is
    /// non-zero with a real side. O(orders); for tests/asserts, not the hot path.
    bool invariant_ok() const {
        std::map<Price, Qty, std::greater<Price>> rebuilt_bids;
        std::map<Price, Qty>                      rebuilt_asks;
        for (const auto& kv : orders_) {
            const Order& o = kv.second;
            if (o.shares == 0) return false;             // zeroed orders must be gone
            if (o.side == Side::Buy)       rebuilt_bids[o.price] += o.shares;
            else if (o.side == Side::Sell) rebuilt_asks[o.price] += o.shares;
            else                           return false;  // resting order needs a side
        }
        if (rebuilt_bids.size() != bids_.size()) return false;
        if (rebuilt_asks.size() != asks_.size()) return false;
        for (const auto& kv : rebuilt_bids) {
            auto it = bids_.find(kv.first);
            if (it == bids_.end() || it->second != kv.second) return false;
        }
        for (const auto& kv : rebuilt_asks) {
            auto it = asks_.find(kv.first);
            if (it == asks_.end() || it->second != kv.second) return false;
        }
        return true;
    }

private:
    void decrement_level(Side side, Price price, Qty d) {
        if (side == Side::Buy) {
            auto it = bids_.find(price);
            if (it != bids_.end() && (it->second -= d) == 0) bids_.erase(it);
        } else {
            auto it = asks_.find(price);
            if (it != asks_.end() && (it->second -= d) == 0) asks_.erase(it);
        }
    }

    std::unordered_map<OrderRef, Order>      orders_;
    std::map<Price, Qty, std::greater<Price>> bids_;  // highest price first
    std::map<Price, Qty>                      asks_;  // lowest price first
};

}  // namespace hftob
