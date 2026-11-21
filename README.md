# ESPAsyncEmu

Desktop emulation of the ESP8266/ESP32 async networking libraries, built on standalone [Asio](https://think-async.com/Asio/). Run firmware-style `AsyncClient` / `AsyncServer` / `AsyncUDP` code on macOS, Linux and Windows.

- **ESPAsyncTCP**: `AsyncClient`, `AsyncServer`
- **ESPAsyncUDP**: `AsyncUDP`, `AsyncUDPPacket`, `AsyncUDPMessage`
- **ESPAsyncWebServer**: `AsyncWebServer` with routing, request/response objects, static files, uploads, Basic/Digest auth, templates, **WebSocket** and **Server-Sent Events**
- **AsyncJson** (`AsyncJsonResponse`, `AsyncCallbackJsonWebHandler`) on ArduinoJson v6/v7
- Arduino-style `String`, `Print`/`Stream`, `FS`/`LittleFS` and a `WiFi` stub, so unmodified sketches build
- **Device profiles**: run under ESP8266/ESP32-like limits (`ESPASYNCEMU_PROFILE=esp8266`) to catch problems that only show up on the board
- Caller-driven event loop: call `AsyncEmu::poll()` from your own loop; all callbacks run on that thread.

## Build and test

```sh
cmake -S . -B build
cmake --build build -j
cd build && ctest --output-on-failure
```

Asio is fetched automatically by CMake. Link against `ESPAsyncEmu::ESPAsyncEmu`.

## Examples and integrations

- `examples/`: TCP echo server, HTTP hello server, a full web app (`web_server`), self-contained TCP and UDP demos
- `integrations/arduino`: run `setup()`/`loop()` sketches on the desktop (echo sketch and a web sketch)
- `integrations/frame_loop`, `integrations/sdl2`: calling `poll()` once per GUI frame

## Documentation

See the [wiki](wiki/Home.md) (markdown with diagrams). Build a browsable HTML version with `python3 wiki/build.py` or `cmake --build build --target wiki`.
