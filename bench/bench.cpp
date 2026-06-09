// Google Benchmark microbenchmarks for the hot paths: ITCH decode, full
// decode->book replay (single-threaded and pipelined), and the SPSC ring.
//
// CI runs these as a smoke test (tiny --benchmark_min_time) only to prove they
// build and run — GitHub's shared runners are virtualized and not representative
// of trading hardware, so the *numbers* here are not HFT figures. Run on a pinned
// bare-metal box for real p50/p99/p999 (methodology in the README).

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "book/order_book.hpp"
#include "core/spsc_ring.hpp"
#include "feed/framing.hpp"
#include "feed/pipeline.hpp"
#include "feed/synthetic.hpp"
#include "itch/decoder.hpp"
#include "itch/messages.hpp"

using namespace hftob;

// Decode a single ITCH AddOrder, repeatedly. make_synthetic_itch is BinaryFILE-
// framed, so skip the 2-byte length prefix and decode the body.
static void BM_Decode(benchmark::State& state) {
    const std::vector<std::uint8_t> buf  = make_synthetic_itch(1, 7);  // [len][AddOrder]
    const std::size_t               mlen = (static_cast<std::size_t>(buf[0]) << 8) |
                                           static_cast<std::size_t>(buf[1]);
    const std::uint8_t*             body = buf.data() + 2;
    itch::Message                   m;
    for (auto _ : state) {
        benchmark::DoNotOptimize(itch::decode(body, mlen, m));
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Decode);

// Full single-threaded replay of an N-message stream into a fresh book.
static void BM_ReplaySingleThreaded(benchmark::State& state) {
    const std::vector<std::uint8_t> data =
        make_synthetic_itch(static_cast<std::size_t>(state.range(0)));
    std::size_t applied = 0;
    for (auto _ : state) {
        OrderBook book;
        applied = replay_single_threaded(data.data(), data.size(), book);
        benchmark::DoNotOptimize(applied);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(applied));
}
BENCHMARK(BM_ReplaySingleThreaded)->Arg(10000);

// Same replay through the two-stage lock-free pipeline (decode thread + book thread).
static void BM_ReplayPipelined(benchmark::State& state) {
    const std::vector<std::uint8_t> data =
        make_synthetic_itch(static_cast<std::size_t>(state.range(0)));
    std::size_t applied = 0;
    for (auto _ : state) {
        OrderBook book;
        applied = replay_pipelined(data.data(), data.size(), book, 4096);
        benchmark::DoNotOptimize(applied);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(applied));
}
BENCHMARK(BM_ReplayPipelined)->Arg(10000);

// Per-op book mutation cost, building an N-message book into a fresh, reserved
// book each iteration. Templated so the std::map and flat-vector level stores are
// benchmarked head to head (same workload, only the level structure differs).
template <typename Book>
static void BM_BookOps(benchmark::State& state) {
    const std::vector<std::uint8_t> data =
        make_synthetic_itch(static_cast<std::size_t>(state.range(0)));
    std::size_t applied = 0;
    for (auto _ : state) {
        Book book(static_cast<std::size_t>(state.range(0)));
        applied = 0;
        for_each_framed_message(data.data(), data.size(),
                                [&](const itch::Message& m) { book.apply(m); ++applied; });
        benchmark::DoNotOptimize(applied);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(applied));
}
BENCHMARK_TEMPLATE(BM_BookOps, OrderBook)->Arg(10000);      // std::map levels (baseline)
BENCHMARK_TEMPLATE(BM_BookOps, FlatOrderBook)->Arg(10000);  // flat sorted-vector levels

// SPSC ring push+pop round trip (single-threaded — measures the per-op cost).
static void BM_SpscPushPop(benchmark::State& state) {
    SpscRing<int> ring(1024);
    int           v = 0;
    for (auto _ : state) {
        ring.push(42);
        ring.pop(v);
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscPushPop);

BENCHMARK_MAIN();
