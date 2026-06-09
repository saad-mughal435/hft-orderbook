#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hftob {

/// A free-list object pool: stable, contiguous storage for `T` addressed by a
/// 32-bit handle, with O(1) alloc/free and no per-object heap allocation on the
/// steady-state path (freed handles are reused). Storage only grows; a handle
/// stays valid until it is freed.
///
/// Caveat: a reference returned by `operator[]` is invalidated by an `alloc()`
/// that grows the backing vector — never hold a `T&` across an `alloc()`.
template <typename T>
class ObjectPool {
public:
    using Handle = std::uint32_t;

    explicit ObjectPool(std::size_t reserve = 0) {
        slots_.reserve(reserve);
        free_.reserve(reserve);
    }

    Handle alloc() {
        if (!free_.empty()) {
            const Handle h = free_.back();
            free_.pop_back();
            return h;
        }
        slots_.emplace_back();
        return static_cast<Handle>(slots_.size() - 1);
    }
    void free(Handle h) { free_.push_back(h); }

    T&       operator[](Handle h)       { return slots_[h]; }
    const T& operator[](Handle h) const { return slots_[h]; }

    std::size_t live() const { return slots_.size() - free_.size(); }
    std::size_t capacity() const { return slots_.size(); }

private:
    std::vector<T>      slots_;
    std::vector<Handle> free_;
};

}  // namespace hftob
