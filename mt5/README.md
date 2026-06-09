# MetaTrader 5 bridge

Runs the same engine against **MetaTrader 5**: live ticks in, order commands out.
The MT5 terminal is Windows-only and its Expert Advisors are written in MQL5, so
the boundary is a small **versioned NDJSON protocol** over raw TCP — no DLLs, no
ZeroMQ, no native dependencies on either side.

```
  MetaTrader 5 terminal (Windows)              hft-orderbook (Linux/macOS)
 ┌───────────────────────────────┐           ┌──────────────────────────────┐
 │  ITCHBridge.mq5  (Expert)      │  NDJSON   │  mt5d  (bridge server)        │
 │   OnTick ─ MqlTick ──────────────► tick ─────►  parse_tick ─ strategy     │
 │   OrderSend(MqlTradeRequest) ◄─── order ◄─────  on_tick -> Order          │
 │   MqlTradeResult.retcode ────────► ack  ─────►  count / route             │
 └───────────────────────────────┘    TCP    └──────────────────────────────┘
        client (connects)                            server (listens)
```

Transport choice (raw TCP + NDJSON via MQL5's built-in `Socket*` API) and the
alternative ZeroMQ route (`dingmaotu/mql-zmq`, Darwinex DWX pattern) are discussed
in the design notes; raw TCP was chosen to keep the EA dependency-free.

## Protocol

One compact JSON object per line (`\n`-delimited). Every message carries a type
`t` and protocol version `v`. Every order carries a correlation `id` echoed on its
ack. Defined once in [`include/mt5/protocol.hpp`](../include/mt5/protocol.hpp).

| Direction      | `t`         | Payload |
| -------------- | ----------- | ------- |
| EA → engine    | `hello`     | `client`, `account` |
| EA → engine    | `subscribe` | `symbol` |
| EA → engine    | `tick`      | `symbol`, `time`, `bid`, `ask`, `last`, `volume` |
| engine → EA    | `order`     | `id`, `symbol`, `side` (`B`/`S`), `volume`, `price` (`0`=market), `kind` |
| EA → engine    | `ack`       | `id`, `ok`, `retcode` (MT5 `MqlTradeResult.retcode`), `message` |
| engine → EA    | `nop`       | — (a tick that produced no order) |
| engine → EA    | `depth`     | `symbol`, `bids` / `asks` as `[[price, qty], …]` ladders |
| EA → engine    | `bye` / `heartbeat` | session control |

The session is strictly request/response: the EA sends a tick and reads exactly
one reply (`order` or `nop`); if it was an order it sends an `ack`. That keeps both
sides single-threaded and race-free with no locking.

String values are escaped (`\"`, `\\`) and keys are matched only at real
object-key positions, so a key name appearing inside a value is never mis-parsed.

### Depth publishing — closing the loop

`publish_depth(sock, symbol, book, n)` serialises the top-`n` levels of an engine
order book as a `depth` message, so the book the engine **reconstructs from ITCH**
can stream straight out to the MetaTrader side (or any subscriber). Prices are the
engine's integer ticks; `parse_depth` rebuilds the ladders.

### Resilience

The server sets a recv timeout (`SO_RCVTIMEO`) and reclaims a session after a few
idle intervals, so a dead terminal can't pin it — a heartbeating EA stays
connected. `ITCHBridge.mq5` reconnects (re-sending `hello` + `subscribe`) from its
`OnTimer` if the link drops.

## Run the server

```bash
cmake -S . -B build && cmake --build build -j
./build/mt5d 9009        # listens on 127.0.0.1:9009
```

## Install the EA

1. Copy `mt5/ITCHBridge.mq5` into `MQL5/Experts/` in your terminal's data folder
   (MetaEditor ▸ open ▸ **Compile**, or drop it in and refresh Navigator).
2. **Allow the address.** Tools ▸ Options ▸ **Expert Advisors** ▸ tick
   *"Allow algorithmic trading"* and add the `mt5d` host to *"Allow WebRequest /
   modify the list of allowed URLs"* — MT5 only lets `Socket*` connect to
   addresses on that allow-list. For a local bridge add `127.0.0.1`.
3. Attach **ITCHBridge** to a chart; set `InpHost` / `InpPort` to match `mt5d`.
   Confirm "ITCHBridge connected" in the Experts log.

> Live `OrderSend` places **real trades** on whatever account the terminal is
> logged into. Validate first in the **Strategy Tester** or on a **demo** account.
> `ExampleStrategy` is a trivial mid-move rule for wiring the round trip, not a
> trading signal — replace `on_tick` with your own logic.

## How it's verified without Windows

CI can't run MetaEditor, so the EA ships as a source artifact. The C++ side and the
wire protocol are proven on Linux by:

- **`tests/mt5_codec_tests.cpp`** — encode/parse round-trips for every message type.
- **`tests/mt5_itest.cpp`** — a mock EA over a real loopback TCP socket streams a
  recorded tick tape to `run_bridge`, the strategy emits orders, the mock EA acks
  them, and the test asserts the full ticks → orders → acks round trip.
