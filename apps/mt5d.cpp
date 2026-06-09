// mt5d — the MetaTrader 5 bridge server. Listens on a loopback TCP port for the
// ITCHBridge.mq5 Expert Advisor, feeds incoming ticks to a strategy, sends order
// commands back, and counts the trade acks the EA returns. Each accepted client
// is served to completion (until it sends `bye` or disconnects), then mt5d waits
// for the next one.
//
//   mt5d [port]        default port 9009
//
// Pair with the EA: set its Host=127.0.0.1 (or this host's address) and Port to
// match, and whitelist the address in MT5 (Tools > Options > Expert Advisors >
// "Allow WebRequest / allow DLL imports" as documented in mt5/README.md).

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "mt5/server.hpp"
#include "mt5/strategy.hpp"
#include "mt5/tcp.hpp"

using namespace hftob;
using namespace hftob::mt5;

int main(int argc, char** argv) {
    std::uint16_t port = 9009;
    if (argc >= 2) {
        const long p = std::strtol(argv[1], nullptr, 10);
        if (p <= 0 || p > 65535) {
            std::cerr << "usage: mt5d [port]\n";
            return 2;
        }
        port = static_cast<std::uint16_t>(p);
    }

    try {
        Listener listener(port);
        std::cout << "mt5d listening on 127.0.0.1:" << listener.port()
                  << "  (Ctrl-C to stop)\n";

        for (;;) {
            LineSocket conn = listener.accept();
            std::cout << "client connected\n";

            ExampleStrategy strat;
            // Reclaim the session if the EA goes silent for ~60s (12 x 5s), so a
            // dead terminal can't pin the server. A heartbeating EA stays connected.
            const BridgeStats st = run_bridge(conn, strat, /*idle_ms=*/5000, /*max_idle=*/12);

            std::cout << "session ended: ticks=" << st.ticks
                      << " orders=" << st.orders
                      << " acks=" << st.acks
                      << " nops=" << st.nops << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "mt5d error: " << e.what() << "\n";
        return 1;
    }
}
