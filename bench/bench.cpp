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
#include "feed/pipeline.hpp"
#include "feed/synthetic.hpp"
#include "itch/decoder.hpp"
#include "itch/messages.hpp"

using namespace hftob;

// Decode a single ITCH AddOrder, repeatedly.
static void BM_Decode(benchmark::State& state) {
    const std::vector<std::uint8_t> buf  = make_synthetic_itch(1, 7);  // 1 AddOrder
    const std::size_t               mlen = itch::message_length(static_cast<char>(buf[0]));
    itch::Message                   m;
    for (auto _ : state) {
        benchmark::DoNotOptimize(itch::decode(buf.data(), mlen, m));
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
