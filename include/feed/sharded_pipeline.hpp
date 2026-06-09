#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "book/book_set.hpp"
#include "core/spsc_ring.hpp"
#include "feed/framing.hpp"
#include "itch/messages.hpp"

namespace hftob {

/// Replay a BinaryFILE-framed ITCH stream across `n_workers` **shards** - the
/// standard way market-data systems scale past one book thread.
///
///   producer (calling) thread : deframe + decode -> push to ring[shard(locate)]
///   worker w                  : pop ring[w] -> apply to its own BookSet
///
/// Symbols are partitioned by `stock_locate`, so each ring is strictly
/// single-producer / single-consumer (reusing `SpscRing`) and each symbol's book
/// is touched by exactly one worker - no locking. The shard books are disjoint, so
/// they merge into `out` after the workers join. Because per-symbol message order
/// is preserved within its shard, the result is identical to a single-threaded
/// `BookSet` replay (parity-tested). Returns the number of messages applied.
inline std::size_t replay_sharded(const std::uint8_t* buf, std::size_t len,
                                  BookSet& out, unsigned n_workers) {
    if (n_workers < 1) n_workers = 1;
    const unsigned W = n_workers;

    std::vector<std::unique_ptr<SpscRing<itch::Message>>> rings;
    rings.reserve(W);
    for (unsigned i = 0; i < W; ++i)
        rings.push_back(std::make_unique<SpscRing<itch::Message>>(4096));

    std::vector<BookSet>     shard_books(W);
    std::atomic<bool>        done{false};
    std::vector<std::thread> workers;
    workers.reserve(W);

    for (unsigned w = 0; w < W; ++w) {
        workers.emplace_back([w, &rings, &shard_books, &done] {
            itch::Message msg;
            for (;;) {
                if (rings[w]->pop(msg)) {
                    shard_books[w].apply(msg);
                } else if (done.load(std::memory_order_acquire)) {
                    if (!rings[w]->pop(msg)) break;   // drain, then exit
                    shard_books[w].apply(msg);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::size_t n = 0;
    for_each_framed_message(buf, len, [&](const itch::Message& m) {
        const unsigned shard =
            static_cast<unsigned>((static_cast<unsigned>(m.stock_locate) * 2654435761u) % W);
        while (!rings[shard]->push(m)) std::this_thread::yield();  // back-pressure
        ++n;
    });
    done.store(true, std::memory_order_release);

    for (auto& t : workers) t.join();
    for (unsigned w = 0; w < W; ++w) out.merge_from(shard_books[w]);
    return n;
}

}  // namespace hftob
