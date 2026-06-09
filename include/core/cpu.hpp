#pragma once

#include <cstdint>

#if !defined(_WIN32)
#include <pthread.h>
#include <sched.h>
#endif

namespace hftob {

/// A spin-loop **pause** hint. During a busy-wait it yields execution resources to
/// a hyperthread sibling and cuts power, *without* giving up the core (unlike
/// `std::this_thread::yield()`, which invites a context switch). The canonical
/// low-latency busy-wait primitive — used in the SPSC ring's spin paths.
inline void cpu_relax() {
#if defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");  // portable compiler barrier
#endif
}

/// Read the CPU timestamp counter (cycle count) — the lowest-overhead clock for
/// hot-path timing on a pinned core. Convert to nanoseconds with the measured TSC
/// frequency (see `docs/PERFORMANCE.md`). Returns 0 where unavailable.
inline std::uint64_t rdtsc() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned hi, lo;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
#else
    return 0;
#endif
}

/// Pin the calling thread to a single core. Real deployments isolate cores
/// (`isolcpus`) and pin the hot threads so they never migrate or get preempted.
/// Returns false where unsupported or if it failed.
inline bool pin_this_thread(unsigned core) {
#if !defined(_WIN32)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)core;
    return false;
#endif
}

}  // namespace hftob
