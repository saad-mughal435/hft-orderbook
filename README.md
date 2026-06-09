# hft-orderbook

A low-latency **C++17** market-data engine that reconstructs a live limit-order book
from the real **NASDAQ TotalView-ITCH 5.0** feed, with a pluggable adapter layer so the
same engine also runs against **MetaTrader 5** (live ticks in / orders out).

[![CI](https://github.com/saad-mughal435/hft-orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/saad-mughal435/hft-orderbook/actions/workflows/ci.yml)

> **Status: feature-complete.** ITCH 5.0 decoder ✅ · order-book reconstructor ✅ · lock-free SPSC
> pipeline ✅ · benchmarks + replay ✅ · MT5 bridge ✅ — built phase by phase, each verified in CI.

## Why a *reconstructor*, not a matching engine

ITCH is **order-based**: every message carries an explicit 8-byte order reference and the
exchange has **already matched**. So this engine never walks the book to match — it applies
deterministic mutations (add / execute / cancel / delete / replace) keyed by order reference.
The dominant data structure is therefore an **O(1) `order_ref → order` map**, which makes the
design both simpler and faster than a generic matching engine.

## Design

| Module | Responsibility |
| ------ | -------------- |
| `core/`  | integer prices, `ObjectPool`, pluggable level stores (`MapLevels` / `FlatLevels`), `alignas(64)` lock-free SPSC ring, latency histogram |
| `itch/`  | ITCH 5.0 message decode (manual big-endian) |
| `feed/`  | BinaryFILE deframer, two-stage decode→book pipeline, deterministic synthetic-capture generator |
| `book/`  | order-book reconstructor (pooled `order_ref → order`) + multi-symbol `BookSet` routed by `stock_locate` |
| `mt5/`   | NDJSON bridge protocol (ticks / orders / acks + **depth publish**) + TCP server + mock client; `ITCHBridge.mq5` EA |
| `bench/` | Google Benchmark microbenchmarks (decode / book / SPSC) |
| `apps/`  | `obreplay` (replay a capture, multi-symbol), `gencap` (write a capture), `mt5d` (MT5 bridge server) |

The ITCH decoder extracts every field by **explicit big-endian byte assembly** — never by
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

NASDAQ publishes full-day TotalView-ITCH 5.0 samples (≈ 3.5–5.6 GB each, no login) at
`https://emi.nasdaq.com/ITCH/Nasdaq ITCH/` as `MMDDYYYY.NASDAQ_ITCH50.gz`. Those are **not**
committed; instead the repo ships a deterministic synthetic-capture generator (`gencap`) and an
`obreplay --synthetic N` mode for tests and the demo, and `obreplay` can be pointed at a real `.gz`.
Both the real files and the synthetic generator use the **BinaryFILE** layout — every message is
preceded by a 2-byte big-endian length — which the engine deframes (`feed/framing.hpp`).

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
two implementations are compared head to head on the same workload — `MapLevels` (a `std::map`
red-black tree) and `FlatLevels` (a cache-friendly sorted vector):

| benchmark (GitHub CI runner — **relative only**) | result |
| --- | --- |
| decode one ITCH message | ~5 ns |
| SPSC ring push + pop | ~3 ns |
| build a 10k-order book — `MapLevels` (std::map) | ~90 ns/msg (≈11 M msg/s) |
| build a 10k-order book — `FlatLevels` (sorted vector) | ~120 ns/msg (≈8 M msg/s) |

On this synthetic feed the `std::map` baseline is **~25–30% faster** than the flat-vector levels: the
mid random-walks across ~100 price levels, so the flat vector pays an O(n) tail-shift on inserts and
erases in the *middle* of the book, while the tree updates scattered levels in O(log n). Flat levels
win the opposite workload — activity tightly clustered at the inside (a few levels, top-of-book
churn), where front/back edits shift little. The engine ships **both**, parity-tested, so the
trade-off is *measured*, not assumed.

> These are **relative, same-runner** figures on virtualized CI hardware — fair for an A/B, not HFT
> numbers. Absolute p50/p99/p999 require pinning threads on bare metal; the method is documented —
> run it on a real box. No fabricated figures here.

## Quality

Every push runs, in GitHub Actions:

- **build + test** (`ctest`, gcc) — unit, property/invariant, multi-symbol replay, and the MT5
  loopback bridge integration test
- **ThreadSanitizer** over the SPSC / pipeline / bridge threads — the proof the lock-free ring is race-free
- **AddressSanitizer + UndefinedBehaviorSanitizer** over the suite and a replay run
- **clang `-Wall -Wextra -Wpedantic -Werror`** build of the library and tools
- **libFuzzer** feeding arbitrary bytes through the deframer + decoder + book (ASan-checked)
- **benchmark smoke** so the benches keep building and running

## License

MIT © Muhammad Saad
