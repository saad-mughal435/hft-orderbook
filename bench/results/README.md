# Latency baseline (recorded CI run)

`latency-baseline.json` is a **real, recorded GitHub Actions run** of the
per-ITCH-message-type decode-to-book latency histogram - not a hand-written or
estimated file. It is committed here so the numbers are visible and reproducible
in-repo; every CI run also re-publishes a fresh copy as the `latency-histogram`
build artifact (see `.github/workflows/ci.yml`, `benchmarks` job).

## What it measures

For a deterministic synthetic NASDAQ ITCH 5.0 session (fixed `seed=1`, so reruns
are directly comparable), it times **only `itch::decode` + `OrderBook::apply` per
message** with `steady_clock` - no file IO, no allocation, no setup inside the
timed region - and records p50/p95/p99/max nanoseconds bucketed by message type
(Add / Execute / Cancel / Delete / Replace) plus an overall histogram.

## Honesty caveat

These are **relative figures from a virtualized, shared CI runner**
(`ubuntu-latest`). They are fair for an A/B and for catching regressions across
runs on the same class of hardware - they are **not** absolute trading-hardware
numbers. Run the same target on a pinned bare-metal box for representative
p50/p99/p999 (methodology in the top-level README).

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHFTOB_BUILD_BENCH=ON -DHFTOB_BUILD_TESTS=OFF
cmake --build build -j
./build/latency_json 200000 latency.json   # arg1 = sample messages, arg2 = output file
cat latency.json
```

Omit the output-file argument to print the JSON to stdout. Set
`HFTOB_GENERATED_AT` (ISO-8601) to stamp a `generated_at` field, as CI does.
