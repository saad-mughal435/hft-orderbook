# hft-orderbook

A low-latency **C++17** market-data engine that reconstructs a live limit-order book
from the real **NASDAQ TotalView-ITCH 5.0** feed, with a pluggable adapter layer so the
same engine also runs against **MetaTrader 5** (live ticks in / orders out).

[![CI](https://github.com/saad-mughal435/hft-orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/saad-mughal435/hft-orderbook/actions/workflows/ci.yml)

> **Status: in progress.** ITCH 5.0 decoder ✅ · order-book reconstructor · lock-free SPSC
> pipeline · benchmarks + replay · MT5 bridge — built phase by phase, each verified in CI.

## Why a *reconstructor*, not a matching engine

ITCH is **order-based**: every message carries an explicit 8-byte order reference and the
exchange has **already matched**. So this engine never walks the book to match — it applies
deterministic mutations (add / execute / cancel / delete / replace) keyed by order reference.
The dominant data structure is therefore an **O(1) `order_ref → order` map**, which makes the
design both simpler and faster than a generic matching engine.

## Design

| Module | Responsibility |
| ------ | -------------- |
| `core/`  | integer prices, object pool, `alignas(64)` lock-free SPSC ring |
| `itch/`  | ITCH 5.0 message decode (manual big-endian) + BinaryFILE deframer |
| `book/`  | order-book reconstructor — `order_ref → order` map, tick-indexed levels |
| `mt5/`   | NDJSON bridge protocol + TCP server + mock client; `ITCHBridge.mq5` EA |
| `bench/` | Google Benchmark microbenchmarks (decode / book / SPSC) |
| `apps/`  | `obreplay` (replay an ITCH `.gz`), `mt5d` (MT5 bridge server) |

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
committed; the repo bundles a tiny synthetic capture for tests and the demo, and `obreplay`
can be pointed at a real `.gz`.

> **A note on latency numbers:** correctness and that the benchmarks build/run are verified in
> CI, but GitHub's shared runners are not representative of trading hardware. Headline
> p50/p99/p999 nanosecond figures require pinning threads on bare metal — the methodology is
> documented; run it on a real box for real numbers (no fabricated figures here).

## License

MIT © Muhammad Saad
