# hft-orderbook

A **C++17** trading-infrastructure project. It reconstructs live limit-order books from
the real **NASDAQ TotalView-ITCH 5.0** feed - over both **BinaryFILE** captures and the **MoldUDP64**
UDP multicast transport - derives microstructure signals, and speaks **FIX 4.4** order entry plus a
**MetaTrader 5** bridge.

[![CI](https://github.com/saad-mughal435/hft-orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/saad-mughal435/hft-orderbook/actions/workflows/ci.yml)

What's in here:

- **Hot path** - lock-free/wait-free SPSC ring, `PAUSE` busy-wait, object-pooled and
  integer-priced mutation path, a price-tick-indexed book. No allocation in steady state;
  the reasoning is in `docs/PERFORMANCE.md`.
- **Market data in** - ITCH 5.0 decode (manual big-endian), the **MoldUDP64** UDP feed with
  sequence-gap detection, and a dependency-free WebSocket publisher.
- **Order entry out** - a **FIX 4.4** codec (NewOrderSingle / ExecutionReport, BodyLength + CheckSum)
  and an NDJSON-over-TCP MetaTrader 5 bridge.
- **Scaling** - symbols **sharded** across worker threads, one SPSC ring per core.
- **Verification** - 6 CI jobs: build+test, **ThreadSanitizer**, ASan/UBSan, clang **`-Werror`**,
  **libFuzzer**, benchmarks.

There is also replay tooling for real and synthetic captures, and a live L2
[browser viewer](https://saadm.dev/hft-book/viewer.html) fed by the engine's WebSocket publisher.

## Why a *reconstructor*, not a matching engine

ITCH is **order-based**: every message carries an explicit 8-byte order reference and the
exchange has **already matched**. So this engine never walks the book to match - it applies
deterministic mutations (add / execute / cancel / delete / replace) keyed by order reference.
The dominant data structure is therefore an **O(1) `order_ref → order` map**, which makes the
design both simpler and faster than a generic matching engine.

## Design

| Module | Responsibility |
| ------ | -------------- |
| `core/`  | integer prices, `ObjectPool`, pluggable level stores (`MapLevels` / `FlatLevels` / `WindowedLevels`), `alignas(64)` lock-free SPSC ring, `cpu_relax`/`rdtsc`/`pin_this_thread`, latency histogram, SHA-1/base64 |
| `itch/`  | ITCH 5.0 message decode (manual big-endian) |
| `feed/`  | BinaryFILE deframer, **MoldUDP64** UDP feed framing + UDP socket, two-stage + **sharded** decode→book pipelines, dependency-free WebSocket codec, synthetic generators |
| `book/`  | order-book reconstructor (pooled `order_ref → order`) + multi-symbol `BookSet` (by `stock_locate`) + **microstructure metrics** + **trade tape** (VWAP / OHLCV) |
| `fix/`   | **FIX 4.4** order-entry codec - tags/enums + message builder/parser (auto BodyLength + CheckSum), NewOrderSingle / ExecutionReport |
| `mt5/`   | NDJSON bridge protocol (ticks / orders / acks + **depth** & **signal** publish) + TCP server + mock client; `ITCHBridge.mq5` EA |
| `bench/` | Google Benchmark microbenchmarks (decode / book / SPSC) |
| `apps/`  | `obreplay` (multi-symbol, `--threads`, signals), `mdrecv` (MoldUDP64 UDP), `wsbook` (WebSocket L2), `mt5d` (bridge), `fixsim`, `gencap` |

The ITCH decoder extracts every field by **explicit big-endian byte assembly** - never by
casting a packed struct over the wire (the layouts are big-endian with misaligned multi-byte
fields, so a cast would be undefined behaviour and wrong on little-endian hosts).

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires CMake ≥ 3.16 and a C++17 compiler. Tests use
[Catch2](https://github.com/catchorg/Catch2) (fetched automatically).

## Market data

NASDAQ publishes full-day TotalView-ITCH 5.0 samples (≈ 3.5-5.6 GB each, no login) at
`https://emi.nasdaq.com/ITCH/Nasdaq ITCH/` as `MMDDYYYY.NASDAQ_ITCH50.gz`. Those are **not**
committed; instead the repo ships a deterministic synthetic-capture generator (`gencap`) and an
`obreplay --synthetic N` mode for tests and the demo, and `obreplay` can be pointed at a real `.gz`.
Both the real files and the synthetic generator use the **BinaryFILE** layout - every message is
preceded by a 2-byte big-endian length - which the engine deframes (`feed/framing.hpp`).

A real NASDAQ day interleaves thousands of symbols in one stream; `obreplay` routes them by
`stock_locate` into a per-symbol `BookSet`, names the books from the StockDirectory (`R`)
messages, and prints the busiest symbols' top-of-book plus a depth ladder.

```bash
./build/obreplay --synthetic 1000000         # replay 1M generated messages
./build/gencap 50000 sample.itch             # write a small reproducible capture
./build/obreplay sample.itch                 # replay it: throughput + latency histogram
./build/obreplay 01302020.NASDAQ_ITCH50.gz   # ...or a real NASDAQ day (zlib build)
./build/obreplay 01302020.NASDAQ_ITCH50.gz --symbol AAPL   # focus one ticker's depth
```

## Performance

Microbenchmarks ([Google Benchmark](https://github.com/google/benchmark), `-DHFTOB_BUILD_BENCH=ON`)
cover the decode and the per-op book mutation. The book is templated over its price-level store, so
**three** implementations are compared head to head on the *same* workload: `MapLevels` (a `std::map`
red-black tree), `FlatLevels` (a cache-friendly sorted vector), and `WindowedLevels` (a price-tick-
indexed array windowed around the inside - the canonical L2 structure):

| benchmark (GitHub CI runner - **relative only**) | result |
| --- | --- |
| decode one ITCH message | ~5 ns |
| SPSC ring push + pop | ~3 ns |
| build a 10k-order book - `MapLevels` (std::map) | ~89 ns/msg (≈11.2 M msg/s) |
| build a 10k-order book - `FlatLevels` (sorted vector) | ~89 ns/msg (≈11.3 M msg/s) |
| build a 10k-order book - `WindowedLevels` (tick-indexed array) | **~71 ns/msg (≈14.0 M msg/s)** |

The **windowed array wins** - ~24% faster than the `std::map` baseline on this run - because an order
keyed by price tick is an O(1) array index (and the best quote is a tracked index), versus the tree's
O(log n) per op. `MapLevels` and `FlatLevels` land close here; their *relative* order is not stable
across runs (an earlier run had flat ~25% slower - the flat vector pays an O(n) tail-shift when the mid
walks across many levels). That instability is the point of reporting **same-run** numbers: the engine
ships all three, parity-tested (`map ≡ flat ≡ windowed`), so the trade-offs are *measured*, not assumed.

A full **decode + book apply is well under 100 ns** on this runner - sub-microsecond per message.
The hot path is allocation-free (object pool + reserved index), integer-priced, branch-light, and
cache-line-disciplined; the lock-free ring busy-waits with `PAUSE` (`cpu_relax`), and the engine
exposes `rdtsc` + `pin_this_thread` for a pinned bare-metal deployment. See
**[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md)** for the hot-path design and the measurement method.

> These are **relative, same-runner** figures on virtualized CI hardware - fair for an A/B, not HFT
> numbers. Absolute p50/p99/p999 require pinning threads on bare metal (isolcpus, fixed TSC) - the
> method is documented; run it on a real box.

## Analytics & live view

A reconstructed book is only useful if it produces something. `book/metrics.hpp` derives the signals
a desk actually watches - **micro-price** (size-weighted fair value), **order-book imbalance** (top
and N-level), and the **spread** in ticks and bps - as pure functions over any book. `book/tape.hpp`
tracks the **trade tape** (last, cumulative volume, **VWAP**, OHLCV bars) from the ITCH `P`/`Q`
prints, distinct from the resting-liquidity book. `obreplay` prints both per symbol (`--bars` for the
OHLCV ladder), and the MT5 bridge can publish them as `signal` messages.

**Scaling.** `feed/sharded_pipeline.hpp` partitions symbols across `W` worker threads by
`hash(stock_locate)`, each owning its books behind its own strictly-SPSC ring - the standard
market-data scaling pattern. `obreplay --threads W --symbols K` runs it; it's parity-checked against
the single-threaded path and ThreadSanitizer-gated.

**Watch it live.** `wsbook` streams conflated L2 depth + signals as JSON over a **dependency-free
WebSocket** (hand-rolled SHA-1 + RFC-6455 frame codec - no libraries), to a small browser
[book viewer](https://saadm.dev/hft-book/viewer.html). `wsbook --dump N` records a real replay to
NDJSON (that's how the viewer's bundled snapshot stream is produced - genuine engine output, not a
mock).

## Quality

Every push runs, in GitHub Actions:

- **build + test** (`ctest`, gcc) - unit, property/invariant, multi-symbol replay, and the MT5
  loopback bridge integration test
- **ThreadSanitizer** over the SPSC / pipeline / bridge threads - the proof the lock-free ring is race-free
- **AddressSanitizer + UndefinedBehaviorSanitizer** over the suite and a replay run
- **clang `-Wall -Wextra -Wpedantic -Werror`** build of the library and tools
- **libFuzzer** feeding arbitrary bytes through the deframer + decoder + book (ASan-checked)
- **benchmark smoke** so the benches keep building and running

## License

MIT © Muhammad Saad
