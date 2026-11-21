# Architecture

## Layers

```mermaid
flowchart TB
    app["Application / firmware-style code<br/>(AsyncClient, AsyncServer, AsyncUDP)"]
    subgraph lib["ESPAsyncEmu"]
        tcp["ESPAsyncTCP.h<br/>src/AsyncTCP.cpp"]
        udp["ESPAsyncUDP.h<br/>src/AsyncUDP.cpp"]
        loop["AsyncEmu.h<br/>src/loop.cpp"]
        ip["IPAddress.h"]
    end
    asio["Asio io_context (single shared instance)"]
    os["OS sockets / timers"]

    app --> tcp
    app --> udp
    app --> loop
    tcp --> loop
    udp --> loop
    tcp --> asio
    udp --> asio
    loop --> asio
    asio --> os
```

Public headers live in `include/` and never expose Asio types. Asio is a private dependency of the static library, so applications only see the Arduino-style API.

## Threading model

Everything is **single threaded and caller-driven**. There is exactly one `asio::io_context` (concurrency hint 1). Nothing happens on the network until the application calls `AsyncEmu::poll()` / `pollFor()` / `run()`; every callback then runs inside that call, on the caller's thread. User code never needs locks, which mirrors the cooperative model of the real ESP libraries.

```mermaid
sequenceDiagram
    participant App as Application loop
    participant Pump as AsyncEmu poll()
    participant Asio as io_context
    participant CB as User callback

    App->>Pump: poll()
    Pump->>Asio: poll() / run_for()
    Asio->>CB: onData / onConnect / onPacket ...
    CB-->>Asio: returns
    Asio-->>Pump: handlers run
    Pump-->>App: count
```

## Object lifetime

On the device, applications routinely `delete` an `AsyncClient` from inside one of its own callbacks (usually `onDisconnect`). With Asio, handlers may still be queued for that object, so each networking object is split in two:

- the **public object** (`AsyncClient`, `AsyncServer`, `AsyncUDP`) that the application owns, and
- a hidden **`Impl`** held by `std::shared_ptr`, which pending Asio handlers also hold.

```mermaid
flowchart LR
    subgraph app["Application"]
        c["AsyncClient"]
    end
    subgraph hidden["Shared state"]
        impl["Impl<br/>socket, timers, buffers, callbacks<br/>owner*, gen"]
    end
    h1["pending read handler"]
    h2["pending write handler"]
    h3["timer handler"]

    c -- "shared_ptr" --> impl
    h1 -- "shared_ptr" --> impl
    h2 -- "shared_ptr" --> impl
    h3 -- "shared_ptr" --> impl
    impl -.->|"owner pointer, nulled in the destructor"| c
```

Two rules make this safe:

1. **`owner` pointer.** `~AsyncClient` sets `Impl::owner = nullptr` and closes the socket silently. After every callback, the code re-checks `owner` before touching anything else.
2. **Generation counter (`gen`).** Every teardown increments `gen`. Handlers capture the value at creation and return immediately if it changed. This prevents stale completions (for example data that arrived just before `close()` + `connect()` on the same object) from being delivered.

Callbacks are *copied* before being invoked, so a callback that re-registers itself (`onData(...)` inside `onData`) does not destroy the running `std::function`.

`AsyncServer` hands out heap-allocated clients exactly like the real library; the application owns them.

## Source map

| File | Purpose |
|------|---------|
| `include/AsyncEmu.h`, `src/loop.cpp` | Event loop control |
| `include/ESPAsyncTCP.h`, `src/AsyncTCP.cpp` | `AsyncClient`, `AsyncServer` |
| `include/ESPAsyncUDP.h`, `src/AsyncUDP.cpp` | `AsyncUDP`, `AsyncUDPPacket`, `AsyncUDPMessage` |
| `include/IPAddress.h` | Arduino-style IPv4 address |
| `src/AsyncEmuInternal.h` | Shared `io_context` accessor, IPAddress/Asio conversions |
| `cmake/FetchAsio.cmake` | Fetches standalone Asio 1.30.2 |
