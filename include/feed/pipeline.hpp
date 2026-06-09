#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "book/order_book.hpp"
#include "core/spsc_ring.hpp"
#include "itch/decoder.hpp"
#include "itch/messages.hpp"

namespace hftob {

/// Walk a contiguous ITCH 5.0 byte stream, decoding each message by its
/// type-derived length and invoking `sink(const itch::Message&)` for every
/// message that decodes. Stops at the first unrecognised type or a truncated
/// tail (both are returned as "bytes consumed < len"). Returns the number of
/// bytes consumed. ITCH messages are self-describing — the type byte determines
/// the length — so no external framing is needed for a back-to-back stream.
template <typename Sink>
std::size_t for_each_message(const std::uint8_t* buf, std::size_t len, Sink&& sink) {
    std::size_t off = 0;
    while (off < len) {
        const std::size_t mlen = itch::message_length(static_cast<char>(buf[off]));
        if (mlen == 0 || off + mlen > len) break;  // unknown type or truncated tail
        itch::Message m;
        if (itch::decode(buf + off, mlen, m)) sink(m);
        off += mlen;
    }
    return off;
}

/// Reference path: decode and apply on the calling thread. Returns messages applied.
inline std::size_t replay_single_threaded(const std::uint8_t* buf, std::size_t len,
                                          OrderBook& book) {
    std::size_t n = 0;
    for_each_message(buf, len, [&](const itch::Message& m) {
        book.apply(m);
        ++n;
    });
    return n;
}

/// Two-stage lock-free pipeline:
///
///   producer thread : decode ITCH  ->  ring.push(Message)
///   consumer thread : ring.pop()   ->  book.apply()        (this thread)
///
/// The `OrderBook` is touched only by the consumer (the calling thread), so it
/// needs no locking. Because the SPSC ring is strictly FIFO, the consumer applies
/// messages in exactly the producer's order — identical to the single-threaded
/// path (see the parity test). Returns the number of messages applied.
inline std::size_t replay_pipelined(const std::uint8_t* buf, std::size_t len,
                                    OrderBook& book, std::size_t ring_capacity = 1024) {
    SpscRing<itch::Message> ring(ring_capacity);
    std::atomic<bool> producing{true};

    std::thread producer([&] {
        for_each_message(buf, len, [&](const itch::Message& m) {
            while (!ring.push(m)) std::this_thread::yield();  // back-pressure on full
        });
        producing.store(false, std::memory_order_release);    // signal: no more pushes
    });

    std::size_t applied = 0;
    itch::Message m;
    for (;;) {
        if (ring.pop(m)) {
            book.apply(m);
            ++applied;
        } else if (!producing.load(std::memory_order_acquire)) {
            // Producer is done AND every prior push is now visible (acquire). One
            // more pop drains a possible last item; a second failure means empty.
            if (!ring.pop(m)) break;
            book.apply(m);
            ++applied;
        } else {
            std::this_thread::yield();  // ring momentarily empty, producer still live
        }
    }

    producer.join();
    return applied;
}

}  // namespace hftob
