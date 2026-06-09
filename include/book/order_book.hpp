#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/flat_levels.hpp"
#include "core/object_pool.hpp"
#include "core/types.hpp"
#include "core/windowed_levels.hpp"
#include "itch/messages.hpp"

namespace hftob {

/// A single-instrument limit-order-book reconstructor.
///
/// ITCH is order-based, so the hot path is an O(1) `order_ref -> order` lookup:
/// every Execute / Cancel / Delete / Replace finds its order directly and adjusts
/// the resting price level. Orders live in an `ObjectPool` (contiguous, no
/// per-order heap churn) indexed by a `reserve`-d `order_ref -> handle` map. Price
/// levels live in a pluggable store (`LevelStore`) keyed so the best quote is
/// first; the default `MapLevels` is the readable baseline and `FlatLevels` is the
/// cache-friendly variant - both are exercised by tests and benchmarked head to
/// head (the engine is templated so the A/B is a one-line type change).
template <template <class> class LevelStore = MapLevels>
class BasicOrderBook {
public:
    struct Order {
        Side  side   = Side::None;
        Price price  = 0;
        Qty   shares = 0;
    };

    explicit BasicOrderBook(std::size_t reserve_orders = 0) : orders_(reserve_orders) {
        if (reserve_orders) index_.reserve(reserve_orders);
    }

    /// Add a resting order (ITCH Add / Add-with-MPID).
    void add(OrderRef ref, Side side, Price price, Qty shares) {
        if (shares == 0) return;
        const std::uint32_t h = orders_.alloc();
        orders_[h] = Order{side, price, shares};   // assign after alloc (no live ref held)
        index_[ref] = h;
        if (side == Side::Buy) bids_.inc(price, shares);
        else                   asks_.inc(price, shares);
    }

    /// Reduce an order by `qty` (ITCH Order Executed / Cancel - partial). Removes
    /// the order if fully consumed. Returns false if the ref is unknown.
    bool reduce(OrderRef ref, Qty qty) {
        auto it = index_.find(ref);
        if (it == index_.end()) return false;
        Order&    o = orders_[it->second];
        const Qty d = (qty < o.shares) ? qty : o.shares;
        o.shares -= d;
        dec_level(o.side, o.price, d);
        if (o.shares == 0) {
            orders_.free(it->second);
            index_.erase(it);
        }
        return true;
    }

    /// Remove an order entirely (ITCH Order Delete). Returns false if unknown.
    bool remove(OrderRef ref) {
        auto it = index_.find(ref);
        if (it == index_.end()) return false;
        const Order o = orders_[it->second];   // copy before freeing the slot
        dec_level(o.side, o.price, o.shares);
        orders_.free(it->second);
        index_.erase(it);
        return true;
    }

    /// Replace an order (ITCH Order Replace): delete the original ref, add the new
    /// ref at a new price/size inheriting the original's side. Loses time priority,
    /// exactly as the exchange models it. Returns false if the original is unknown.
    bool replace(OrderRef orig_ref, OrderRef new_ref, Qty new_shares, Price new_price) {
        auto it = index_.find(orig_ref);
        if (it == index_.end()) return false;
        const Side side = orders_[it->second].side;  // read side before erasing
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
    bool best_bid(Price& price, Qty& qty) const { return bids_.best(price, qty); }
    bool best_ask(Price& price, Qty& qty) const { return asks_.best(price, qty); }
    Qty  qty_at(Side side, Price price) const {
        return (side == Side::Buy) ? bids_.at(price) : asks_.at(price);
    }
    std::size_t order_count() const { return index_.size(); }
    std::size_t bid_levels()  const { return bids_.size(); }
    std::size_t ask_levels()  const { return asks_.size(); }

    /// Top-`n` price levels on a side, best first (bids descending, asks ascending).
    std::vector<std::pair<Price, Qty>> depth(Side side, std::size_t n) const {
        return (side == Side::Buy) ? bids_.top(n) : asks_.top(n);
    }
    std::vector<std::pair<Price, Qty>> bids(std::size_t n) const { return bids_.top(n); }
    std::vector<std::pair<Price, Qty>> asks(std::size_t n) const { return asks_.top(n); }

    /// Recompute every level's quantity from the order map and check it matches the
    /// incrementally-maintained level totals - the core consistency invariant.
    /// O(orders); for tests/asserts, not the hot path.
    bool invariant_ok() const {
        std::map<Price, Qty, std::greater<Price>> rebuilt_bids;
        std::map<Price, Qty>                      rebuilt_asks;
        for (const auto& kv : index_) {
            const Order& o = orders_[kv.second];
            if (o.shares == 0) return false;
            if (o.side == Side::Buy)       rebuilt_bids[o.price] += o.shares;
            else if (o.side == Side::Sell) rebuilt_asks[o.price] += o.shares;
            else                           return false;
        }
        if (rebuilt_bids.size() != bids_.size()) return false;
        if (rebuilt_asks.size() != asks_.size()) return false;
        for (const auto& kv : rebuilt_bids)
            if (bids_.at(kv.first) != kv.second) return false;
        for (const auto& kv : rebuilt_asks)
            if (asks_.at(kv.first) != kv.second) return false;
        return true;
    }

private:
    void dec_level(Side side, Price price, Qty d) {
        if (side == Side::Buy) bids_.dec(price, d);
        else                   asks_.dec(price, d);
    }

    LevelStore<std::greater<Price>> bids_;   // highest price first
    LevelStore<std::less<Price>>    asks_;   // lowest price first
    ObjectPool<Order>               orders_;
    std::unordered_map<OrderRef, std::uint32_t> index_;  // order_ref -> pool handle
};

/// Default book: std::map levels (readable baseline). `FlatOrderBook` swaps in the
/// cache-friendly sorted-vector levels and `WindowedOrderBook` the price-tick-indexed
/// array - the three are benchmarked head to head (`BM_BookOps`).
using OrderBook         = BasicOrderBook<MapLevels>;
using FlatOrderBook     = BasicOrderBook<FlatLevels>;
using WindowedOrderBook = BasicOrderBook<WindowedLevels>;

}  // namespace hftob
