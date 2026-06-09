#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace hftob {

/// Collects per-sample latency measurements (nanoseconds) and reports the
/// distribution. HFT cares about *tails*, so this surfaces p50/p99/p99.9/max -
/// not just a mean - plus a compact log2 ASCII histogram. Percentiles are taken
/// by sorting a copy: fine for an offline replay/report tool, never on a hot path.
class LatencyHist {
public:
    void        add(std::uint64_t ns) { samples_.push_back(ns); }
    std::size_t count() const { return samples_.size(); }

    std::uint64_t percentile(double p) const {
        if (samples_.empty()) return 0;
        std::vector<std::uint64_t> s = samples_;
        std::sort(s.begin(), s.end());
        std::size_t idx = static_cast<std::size_t>(p / 100.0 * (s.size() - 1) + 0.5);
        if (idx >= s.size()) idx = s.size() - 1;
        return s[idx];
    }
    std::uint64_t max() const {
        return samples_.empty() ? 0 : *std::max_element(samples_.begin(), samples_.end());
    }
    double mean() const {
        if (samples_.empty()) return 0.0;
        long double sum = 0;
        for (auto v : samples_) sum += v;
        return static_cast<double>(sum / static_cast<long double>(samples_.size()));
    }

    void print(std::ostream& os, const std::string& label) const {
        os << label << "  (n=" << samples_.size() << ")\n";
        os << "  mean   " << mean()           << " ns\n";
        os << "  p50    " << percentile(50)   << " ns\n";
        os << "  p99    " << percentile(99)   << " ns\n";
        os << "  p99.9  " << percentile(99.9) << " ns\n";
        os << "  max    " << max()            << " ns\n";
        print_histogram(os);
    }

private:
    void print_histogram(std::ostream& os) const {
        if (samples_.empty()) return;
        constexpr int kBuckets = 21;  // 2^0 .. 2^20 ns (~1 ms)
        std::size_t hist[kBuckets] = {};
        std::size_t peak = 0;
        for (auto v : samples_) {
            int           b  = 0;
            std::uint64_t hi = 1;
            while (b < kBuckets - 1 && v >= hi) { hi <<= 1; ++b; }
            ++hist[b];
            if (hist[b] > peak) peak = hist[b];
        }
        os << "  distribution (ns, log2 buckets):\n";
        for (int b = 0; b < kBuckets; ++b) {
            if (hist[b] == 0) continue;
            const std::uint64_t lo   = (b == 0) ? 0 : (std::uint64_t(1) << (b - 1));
            const std::uint64_t hi   = std::uint64_t(1) << b;
            const int           bars = peak ? static_cast<int>(hist[b] * 40 / peak) : 0;
            os << "    [" << lo << "," << hi << ")  ";
            for (int i = 0; i < bars; ++i) os.put('#');
            os << "  " << hist[b] << "\n";
        }
    }

    std::vector<std::uint64_t> samples_;
};

}  // namespace hftob
