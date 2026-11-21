# ESPAsyncEmu

Desktop emulation of the ESP8266/ESP32 async networking libraries, built on standalone [Asio](https://think-async.com/Asio/). Run firmware-style `AsyncClient` / `AsyncServer` / `AsyncUDP` code on macOS, Linux and Windows.

- **ESPAsyncTCP**: `AsyncClient`, `AsyncServer`
- **ESPAsyncUDP**: `AsyncUDP`, `AsyncUDPPacket`, `AsyncUDPMessage`
- Caller-driven event loop: call `AsyncEmu::poll()` from your own loop; all callbacks run on that thread.

## Build and test

```sh
cmake -S . -B build
cmake --build build -j
cd build && ctest --output-on-failure
```

Asio is fetched automatically by CMake. Link against `ESPAsyncEmu::ESPAsyncEmu`.

## Documentation

See the [wiki](wiki/Home.md) (markdown with diagrams). Build a browsable HTML version with `python3 wiki/build.py` or `cmake --build build --target wiki`.
