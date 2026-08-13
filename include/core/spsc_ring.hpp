#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace hftob {

/// Bytes of separation used to keep the producer's and consumer's hot counters
/// apart. 64 is the cache-line size on x86-64 and on most ARM cores.
///
/// This is deliberately *not* `std::hardware_destructive_interference_size`:
/// that constant's value is an ABI commitment, and GCC warns on any use of it
/// that could leak into a public interface (`-Winterference-size`).
///
/// 64 is also not a universal guarantee. Intel's L2 adjacent-line prefetcher can
/// pull in the neighbouring line, and Apple Silicon uses 128-byte lines - which
/// is why Folly and DPDK pad to 128. On those targets this separation *reduces*
/// false sharing rather than eliminating it.
inline constexpr std::size_t kCacheLineBytes = 64;

/// A bounded, wait-free **single-producer / single-consumer** ring buffer.
///
/// This is the hand-off between the decode thread and the book thread: exactly
/// one thread calls `push()`, exactly one calls `pop()`.
///
/// "Wait-free" is the stronger of the two usual guarantees and is the one that
/// applies here: `push()` and `pop()` each complete in a bounded number of steps
/// with no retry loop, so neither side can be delayed by the other. (Every
/// wait-free structure is also lock-free; the reverse does not hold.) The bound
/// only survives if copying `T` is itself bounded - see the `static_assert`.
///
/// Design points that make it fast:
///
///   * **Power-of-two capacity** - the slot index is `counter & mask_`, so the
///     wrap is a single AND instead of a modulo.
///   * **Monotonic counters** - `head_`/`tail_` only ever increase, so `full`
///     (`head - tail == capacity`) and `empty` (`head == tail`) are
///     distinguishable without sacrificing a slot.
///   * **Cache-line separation** - `head_` and `tail_` sit on separate
///     `kCacheLineBytes` lines, so on x86-64 the producer and consumer do not
///     false-share the other side's hot counter.
///   * **Cached opposite index** - each side keeps a private copy of the other's
///     counter and only reloads it (an acquire across cores) when its local copy
///     says the ring looks full/empty, removing most cross-core atomic traffic
///     on the steady-state fast path.
template <typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRing is only wait-free for trivially copyable T: push() and pop() "
                  "move elements by plain copy-assignment, and a T whose assignment "
                  "allocates or takes a lock would forfeit the wait-free bound.");

public:
    explicit SpscRing(std::size_t capacity)
        : cap_(round_up_pow2(capacity)), mask_(cap_ - 1), slots_(cap_) {}

    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    /// Producer side. Returns false (without enqueuing) if the ring is full.
    bool push(const T& item) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;
        if (next - tail_cache_ > cap_) {                 // maybe full -> refresh
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (next - tail_cache_ > cap_) return false;  // genuinely full
        }
        slots_[head & mask_] = item;
        head_.store(next, std::memory_order_release);     // publish the slot
        return true;
    }

    /// Consumer side. Returns false (leaving `out` untouched) if the ring is empty.
    bool pop(T& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_cache_) {                        // maybe empty -> refresh
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail == head_cache_) return false;        // genuinely empty
        }
        out = slots_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release); // release the slot
        return true;
    }

    std::size_t capacity() const { return cap_; }

    /// Approximate occupancy - safe to call from either side (metrics/tests).
    ///
    /// Load `tail_` **first**. Both counters are monotonic and `tail_ <= head_`
    /// always holds, so reading tail first guarantees `t <= h` for the pair this
    /// call actually observed, and the subtraction cannot go negative.
    ///
    /// The other order is subtly wrong: between the two loads the consumer can
    /// advance `tail_` past the `h` already read, and `h - t` on unsigned
    /// `std::size_t` then underflows to ~2^64 - reporting a drained ring as
    /// nearly full, and making `empty_approx()` claim a genuinely empty ring is
    /// not empty. Both loads are correctly synchronised, so this is not a data
    /// race and ThreadSanitizer cannot see it.
    std::size_t size_approx() const {
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t h = head_.load(std::memory_order_acquire);
        return h - t;
    }
    bool empty_approx() const { return size_approx() == 0; }

private:
    static std::size_t round_up_pow2(std::size_t n) {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p < 2 ? 2 : p;  // floor the capacity at 2 slots
    }

    const std::size_t cap_;
    const std::size_t mask_;
    std::vector<T>    slots_;

    // Producer-owned line: head_ (shared) + the producer's private view of tail.
    alignas(kCacheLineBytes) std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_{0};

    // Consumer-owned line: tail_ (shared) + the consumer's private view of head.
    alignas(kCacheLineBytes) std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_{0};
};

}  // namespace hftob
