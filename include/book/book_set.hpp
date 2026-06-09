#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "book/order_book.hpp"
#include "itch/messages.hpp"

namespace hftob {

/// Routes a full **multi-symbol** ITCH 5.0 feed into per-instrument order books.
///
/// Every ITCH message carries a 2-byte `stock_locate` in its common header (the
/// per-day index of the security), so order messages route to the right book by
/// that key alone — no per-message symbol lookup on the hot path. StockDirectory
/// ('R') messages populate the `locate -> symbol` map so a book can also be found
/// by ticker. This is what makes `obreplay` meaningful on a real NASDAQ day, which
/// interleaves thousands of symbols in one stream.
class BookSet {
public:
    void apply(const itch::Message& m) {
        using T = itch::MsgType;
        if (m.type == T::StockDirectory) {
            symbols_[m.stock_locate] = trim(m.stock);
            books_[m.stock_locate];                 // ensure the book exists
            return;
        }
        switch (m.type) {  // order-affecting types route by header stock_locate
            case T::AddOrder:
            case T::AddOrderMpid:
            case T::OrderExecuted:
            case T::OrderExecutedWithPrice:
            case T::OrderCancel:
            case T::OrderDelete:
            case T::OrderReplace:
                books_[m.stock_locate].apply(m);
                break;
            default:
                break;  // SystemEvent / Trade / CrossTrade: tape-only, no book change
        }
    }

    const OrderBook* book(std::uint16_t locate) const {
        auto it = books_.find(locate);
        return (it == books_.end()) ? nullptr : &it->second;
    }
    const OrderBook* book(const std::string& symbol) const {
        for (const auto& kv : symbols_)
            if (kv.second == symbol) return book(kv.first);
        return nullptr;
    }
    std::string symbol(std::uint16_t locate) const {
        auto it = symbols_.find(locate);
        return (it == symbols_.end()) ? std::string() : it->second;
    }

    std::size_t book_count() const { return books_.size(); }
    std::size_t total_orders() const {
        std::size_t n = 0;
        for (const auto& kv : books_) n += kv.second.order_count();
        return n;
    }

    const std::unordered_map<std::uint16_t, OrderBook>& books() const { return books_; }

private:
    static std::string trim(const char (&s)[8]) {  // ITCH symbols are space-padded
        std::size_t n = 8;
        while (n > 0 && s[n - 1] == ' ') --n;
        return std::string(s, n);
    }

    std::unordered_map<std::uint16_t, OrderBook>    books_;
    std::unordered_map<std::uint16_t, std::string>  symbols_;
};

}  // namespace hftob
