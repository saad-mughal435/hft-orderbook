// gencap — write a small, reproducible synthetic NASDAQ ITCH 5.0 capture to disk,
// so the replay demo and tests have a real file to read without the multi-GB
// production sample. Deterministic: same N always yields the same bytes.
//
//   gencap <N messages> <out.itch>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

#include "feed/synthetic.hpp"

using namespace hftob;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: gencap <N messages> <out.itch>\n";
        return 2;
    }
    const long n = std::strtol(argv[1], nullptr, 10);
    if (n <= 0) { std::cerr << "error: N must be positive\n"; return 2; }

    const std::vector<std::uint8_t> data = make_synthetic_itch(static_cast<std::size_t>(n));

    std::ofstream f(argv[2], std::ios::binary);
    if (!f) { std::cerr << "error: cannot write " << argv[2] << "\n"; return 1; }
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    if (!f) { std::cerr << "error: write failed\n"; return 1; }

    std::cout << "wrote " << data.size() << " bytes (" << n << " messages) to "
              << argv[2] << "\n";
    return 0;
}
