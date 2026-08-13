# Performance & latency methodology

This engine is built for **low latency**; the design choices are the standard HFT
ones. This documents what's on the hot path and how to measure it for real.

## What's on the hot path (and what isn't)

- **No allocation per message.** Orders live in an `ObjectPool` (free-list slab);
  the `order_ref → handle` index is `reserve`-d up front; decoding writes into a
  fixed-size `Message`. No `new` / `malloc` in steady state.
- **Integer prices** (`int64` ticks), never floating point on the path.
- **Branch-light decode** by explicit byte offsets - no packed-struct casts, no
  format-string parsing.
- **Cache-line discipline.** The SPSC ring's `head`/`tail` are `alignas(64)` on
  separate lines, so on x86-64 the two sides do not false-share; each side caches
  the opposite index so the steady-state path avoids a contended atomic load.
  (64 is the line size on x86-64 and most ARM, but it is not universal - Intel's
  L2 adjacent-line prefetcher and Apple Silicon's 128-byte lines are why Folly and
  DPDK pad to 128.)
- **Price-tick-indexed book** (`WindowedLevels`): an order update is an O(1) array
  index, and the best quote is a tracked index - no tree walk.
- **Busy-wait with `PAUSE`.** The lock-free ring spins with `cpu_relax()` (x86
  `pause`, `core/cpu.hpp`), not `yield()` - it keeps the core and is friendly to a
  hyperthread sibling.
- **Wait-free** SPSC hand-off - `push()`/`pop()` finish in a bounded number of
  steps with no retry loop, which is strictly stronger than lock-free (the bound
  holds because the ring only accepts trivially copyable elements). No races
  observed under ThreadSanitizer, which is evidence rather than proof: TSan checks
  the interleavings a run happens to take, not that the memory orderings are
  strong enough. **Sharded** across cores by symbol (one SPSC ring per shard, no
  shared mutable state between workers).

## Indicative numbers - relative, same CI runner, NOT HFT figures

| op | ~time |
| -- | ----- |
| decode one ITCH message | ~5 ns |
| book mutation (windowed levels) | ~71 ns/msg |
| SPSC push + pop | ~3 ns |

A full **decode + book apply is well under 100 ns** - sub-microsecond per message
- even on a shared virtualized runner. These are *relative* A/B numbers, not a
latency claim.

## Getting numbers you can stand behind

GitHub's runners are virtualized and shared; real measurement needs a quiet box:

1. **Pin** the hot threads to **isolated** cores - boot with
   `isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3`, then `pin_this_thread(2)` (or
   `taskset -c 2`). `mdrecv --pin <core>` does this for the receive thread.
2. **Disable frequency scaling / turbo** (`cpupower frequency-set -g performance`),
   and prefer an invariant TSC (`constant_tsc`, `nonstop_tsc`).
3. **Time with the TSC** (`rdtsc()`), converting cycles → ns with the measured TSC
   frequency, rather than `clock_gettime` on the hot path.
4. Report the **tail**: p50 / p99 / p99.9 / p99.99 / max - `obreplay` already
   prints a p50/p99/p99.9 histogram (`core/latency_hist.hpp`).
5. Warm the caches, run millions of messages, and discard the first batch.

No fabricated nanosecond figures are published here - run the above on real
hardware for numbers you can defend.
