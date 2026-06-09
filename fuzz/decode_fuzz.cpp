// libFuzzer harness for the ITCH decode + deframe + book-apply path. Feeds
// arbitrary bytes through the BinaryFILE deframer and the decoder and applies the
// results to a book: under AddressSanitizer this proves the decoder never reads
// out of bounds or crashes on malformed/truncated input, however adversarial.
//
//   clang++ -std=c++17 -g -fsanitize=fuzzer,address fuzz/decode_fuzz.cpp \
//           src/itch/decoder.cpp -Iinclude -o decode_fuzz
//   ./decode_fuzz -max_total_time=20 corpus/
//
// (CMake builds it via -DHFTOB_BUILD_FUZZ=ON; CI runs a short session.)

#include <cstddef>
#include <cstdint>

#include "book/book_set.hpp"
#include "feed/framing.hpp"
#include "itch/decoder.hpp"
#include "itch/messages.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace hftob;

    // 1) Treat the input as a BinaryFILE-framed stream: deframe, decode, and route
    //    every message into a multi-symbol book set.
    BookSet book;
    for_each_framed_message(data, size, [&](const itch::Message& m) { book.apply(m); });

    // 2) Also decode the whole buffer as a single message of length `size`, to
    //    exercise the per-type field extraction at arbitrary, unframed lengths.
    if (size > 0) {
        itch::Message m;
        (void)itch::decode(data, size, m);
    }
    return 0;
}
