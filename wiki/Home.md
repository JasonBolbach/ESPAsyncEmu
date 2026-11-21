# ESPAsyncEmu

Desktop emulation of the ESP8266/ESP32 async networking libraries (ESPAsyncTCP, ESPAsyncUDP and ESPAsyncWebServer with WebSocket and Server-Sent Events), built on standalone [Asio](https://think-async.com/Asio/). It lets firmware-style code that uses the `AsyncClient` / `AsyncServer` / `AsyncUDP` APIs run and be tested on macOS, Linux and Windows.

## Pages

| Page | What it covers |
|------|----------------|
| [Architecture](Architecture.md) | Layers, threading model, object lifetime rules |
| [Event Loop](Event-Loop.md) | `AsyncEmu::poll()` and friends, how to drive the emulation |
| [ESPAsyncTCP](ESPAsyncTCP.md) | `AsyncClient` / `AsyncServer`: API, state machine, write path |
| [ESPAsyncUDP](ESPAsyncUDP.md) | `AsyncUDP` / `AsyncUDPPacket` / `AsyncUDPMessage` |
| [ESPAsyncWebServer](ESPAsyncWebServer.md) | `AsyncWebServer`: routing, requests, responses, static files, uploads, auth, templates |
| [Realtime](Realtime.md) | `AsyncWebSocket` and `AsyncEventSource` (Server-Sent Events) |
| [Arduino-Compat](Arduino-Compat.md) | `String`, `FS`/`LittleFS`, `WiFi` stub, Arduino core shim, privileged ports |
| [Examples](Examples.md) | Echo server, HTTP servers, full web app, self-contained demos |
| [Integrations](Integrations.md) | Arduino sketch runner (echo and web sketches), frame-loop pattern, SDL2 |
| [Testing](Testing.md) | Building, running tests, sanitizer runs, test helpers |
| [Roadmap](Roadmap.md) | What is done, what is next, known limitations |

## Quick start

```cpp
#include "ESPAsyncTCP.h"

AsyncServer server(8080);

int main() {
    server.onClient([](void*, AsyncClient* c) {
        c->onData([](void*, AsyncClient* c, void* data, size_t len) {
            c->write(static_cast<const char*>(data), len);   // echo
        });
        c->onDisconnect([](void*, AsyncClient* c) { delete c; });
    });
    server.begin();

    for (;;) AsyncEmu::pollFor(10);   // or call AsyncEmu::poll() once per UI frame
}
```

Link against the `ESPAsyncEmu::ESPAsyncEmu` CMake target; Asio is fetched automatically.

A web server is just as short; see [ESPAsyncWebServer](ESPAsyncWebServer.md):

```cpp
#include "ESPAsyncWebServer.h"

AsyncWebServer server(8080);

int main() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "Hello!"); });
    server.begin();
    for (;;) AsyncEmu::pollFor(50);
}
```

## Maintaining this wiki

The wiki is updated alongside the code: every new feature or behaviour change gets a page or a section, with a diagram where it explains something (state machines, sequences, layering). Diagrams are Mermaid fenced blocks.

### HTML version

The markdown pages are compiled to a browsable static site in `wiki/html/` (open `wiki/html/index.html`):

```sh
python3 wiki/build.py              # or: cmake --build build --target wiki
```

- No Python packages are required; `build.py` has its own small markdown converter.
- Mermaid diagrams are rendered in the browser. The first run downloads a pinned `mermaid.min.js` (v11.4.1) into `wiki/html/assets/`, so the output then works offline.
- Light and dark themes follow the system setting.
- Mermaid pitfalls found so far: `loop`, `end` and similar keywords can't be used as participant/node ids, and `::` in a label breaks sequence diagrams.
