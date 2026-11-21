# Event Loop

Declared in `include/AsyncEmu.h`, implemented in `src/loop.cpp`.

| Function | Behaviour |
|----------|-----------|
| `std::size_t poll()` | Run all ready handlers without blocking. Returns how many ran. The primary "pump" entry point. |
| `std::size_t pollFor(unsigned ms)` | Like `poll()`, but waits up to `ms` for work. |
| `void run()` | Block until `stop()` is called or no work remains (headless tools). |
| `void stop()` | Make `run()` return. Safe to call from a callback. |
| `void post(std::function<void()>)` | Queue a function for the next `poll()`/`run()`. |
| `void reset()` | Drop all pending work and recreate the context (tests only). |
| `void setPortOffset(unsigned)` / `portOffset()` | Offset added to listening ports below 1024 so `server(80)` works without root; see [Arduino-Compat](Arduino-Compat.md#privileged-ports). |

The loop restarts itself automatically after `stop()` or after running out of work, so you can call `poll()` forever without extra bookkeeping.

## Choosing how to drive it

```mermaid
flowchart TD
    q{"What owns the main loop?"}
    q -->|"GUI / game frame loop"| a["call AsyncEmu::poll() once per frame"]
    q -->|"Console tool, wants low CPU"| b["loop on AsyncEmu::pollFor(10)"]
    q -->|"Headless, runs until done"| c["AsyncEmu::run()<br/>+ AsyncEmu::stop() from a callback"]
```

## Notes

- Open sockets with a pending read (every connected client, every listening server, every bound UDP socket) count as work, so `run()` only returns on its own once those are closed. Use `stop()` for long-lived servers.
- `pollFor()` and `run()` only wait while something is pending. With **no** pending work at all (no open sockets or timers) they return immediately, so a loop that relies on `pollFor()` for pacing will spin until a server/socket exists. Hosts that may have no sockets (for example the Arduino runner) sleep for a millisecond between polls instead.
- `reset()` invalidates every live client, server and UDP object. Only use it when none exist.
