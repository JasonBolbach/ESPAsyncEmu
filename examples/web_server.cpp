// A complete web app on AsyncWebServer: static site, JSON API, form POST,
// multipart upload, HTTP auth, WebSocket chat and Server-Sent Events.
//
//   web_server [port] [site-dir]      (defaults: 8080, the bundled examples/web_data)
//   then open http://127.0.0.1:8080/
//
// Uploaded files are stored under ./web_uploads (the emulated SPIFFS).
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "ESPAsyncWebServer.h"
#ifdef ESPASYNCEMU_HAS_ARDUINOJSON
#include "AsyncJson.h"
#endif

#ifndef WEB_DATA_DIR
#define WEB_DATA_DIR "examples/web_data"
#endif

static String jsonEscape(const String& s) {
    String out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // show log lines immediately, even when piped
    uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 8080;
    LittleFS.setRoot(argc > 2 ? argv[2] : WEB_DATA_DIR);  // the site (read-only here)
    SPIFFS.setRoot("./web_uploads");                      // writable storage for uploads
    if (!LittleFS.begin() || !SPIFFS.begin()) {
        std::fprintf(stderr, "file system init failed\n");
        return 1;
    }

    AsyncWebServer server(port);
    AsyncWebSocket ws("/ws");
    AsyncEventSource events("/events");
    const auto startedAt = std::chrono::steady_clock::now();
    auto uptimeSeconds = [&] {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startedAt).count();
    };

    // ---- WebSocket chat: broadcast whatever a client says ----
    ws.onEvent([](AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType type, void* arg, uint8_t* data, size_t len) {
        if (type == WS_EVT_CONNECT) {
            c->printf("welcome, you are client #%u", c->id());
            s->printfAll("* client #%u joined (%u online)", c->id(), static_cast<unsigned>(s->count()));
        } else if (type == WS_EVT_DISCONNECT) {
            s->printfAll("* client #%u left", c->id());
        } else if (type == WS_EVT_DATA) {
            auto* info = static_cast<AwsFrameInfo*>(arg);
            if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
                s->textAll(String("#") + c->id() + ": " + String(reinterpret_cast<char*>(data), len));
            }
        }
    });
    server.addHandler(&ws);
    server.addHandler(&events);

    // ---- JSON API ----
#ifdef ESPASYNCEMU_HAS_ARDUINOJSON
    server.on("/api/status", HTTP_GET, [&](AsyncWebServerRequest* request) {
        AsyncJsonResponse* response = new AsyncJsonResponse();
        JsonVariant& root = response->getRoot();
        root["uptime_s"] = static_cast<long>(uptimeSeconds());
        root["websocket_clients"] = static_cast<unsigned>(ws.count());
        root["event_clients"] = static_cast<unsigned>(events.count());
        root["your_ip"] = request->client()->remoteIP().toString();
        response->setLength();
        request->send(response);
    });

    // POST {"numbers":[1,2,3]} -> {"count":3,"sum":6}  (Content-Type: application/json)
    server.addHandler(new AsyncCallbackJsonWebHandler("/api/sum", [](AsyncWebServerRequest* request, JsonVariant& json) {
        long sum = 0;
        size_t count = 0;
        for (JsonVariant n : json["numbers"].as<JsonArray>()) {
            sum += n.as<long>();
            ++count;
        }
        AsyncJsonResponse* response = new AsyncJsonResponse();
        response->getRoot()["count"] = count;
        response->getRoot()["sum"] = sum;
        response->setLength();
        request->send(response);
    }));
#else
    server.on("/api/status", HTTP_GET, [&](AsyncWebServerRequest* request) {
        String json = String("{\"uptime_s\":") + uptimeSeconds() + ",\"websocket_clients\":" + static_cast<unsigned>(ws.count()) +
                      ",\"event_clients\":" + static_cast<unsigned>(events.count()) + ",\"your_ip\":\"" +
                      request->client()->remoteIP().toString() + "\"}";
        request->send(200, "application/json", json);
    });
#endif

    server.on("/api/echo", HTTP_POST, [](AsyncWebServerRequest* request) {
        String out = "Received form fields:\n";
        for (size_t i = 0; i < request->params(); ++i) {
            AsyncWebParameter* p = request->getParam(i);
            out += String("  ") + p->name() + " = " + p->value() + (p->isPost() ? "  (POST)\n" : "  (GET)\n");
        }
        request->send(200, "text/plain", out);
    });

    server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest* request) {
        String json = "[";
        File dir = SPIFFS.open("/uploads");
        bool first = true;
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            json += String(first ? "" : ",") + "{\"name\":\"" + jsonEscape(f.name()) + "\",\"size\":" + static_cast<unsigned>(f.size()) + "}";
            first = false;
        }
        request->send(200, "application/json", json + "]");
    });

    // ---- multipart upload, streamed to the file system ----
    server.on(
        "/upload", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            AsyncWebParameter* f = request->getParam("file", true, true);
            if (f && f->size()) request->send(200, "text/plain", String("Uploaded ") + f->value() + " (" + static_cast<unsigned>(f->size()) + " bytes)");
            else request->send(400, "text/plain", "No file received");
        },
        [](AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
            // _tempObject holds the open file between chunks (freed with the request)
            if (index == 0) {
                // keep only the last path component: never trust client-supplied paths
                String name = filename.substring(static_cast<size_t>(filename.lastIndexOf('/') + 1));
                if (!name.length()) return;
                auto* f = new File(SPIFFS.open(String("/uploads/") + name, "w"));
                request->_tempObject = std::malloc(sizeof(File*));
                *static_cast<File**>(request->_tempObject) = f;
            }
            if (!request->_tempObject) return;
            File* f = *static_cast<File**>(request->_tempObject);
            if (len) f->write(data, len);
            if (final) {
                f->close();
                delete f;
                *static_cast<File**>(request->_tempObject) = nullptr;
            }
        });

    // ---- HTTP authentication (Basic here; requestAuthentication() defaults to Digest) ----
    server.on("/admin", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!request->authenticate("admin", "secret")) return request->requestAuthentication("ESPAsyncEmu admin", false);
        request->send(200, "text/html", "<h1>Admin</h1><p>You are authenticated.</p><p><a href=\"/\">Back</a></p>");
    });

    // Digest authentication is the default for requestAuthentication()
    server.on("/admin-digest", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!request->authenticate("admin", "secret", "ESPAsyncEmu digest")) return request->requestAuthentication("ESPAsyncEmu digest");
        request->send(200, "text/plain", "digest auth ok");
    });

    // ---- files: the site and the uploads, with a template placeholder in index.html ----
    server.serveStatic("/uploads", SPIFFS, "/uploads/", "no-cache");

    // The start page goes through a template processor (%HOST%); everything else is
    // served as plain, cacheable files (ETag / If-None-Match -> 304).
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(LittleFS, "/index.html", "text/html", false,
                      [](const String& var) { return var == "HOST" ? String("this machine") : String(); });
    });
    server.serveStatic("/", LittleFS, "/", "max-age=60");

    server.onNotFound([](AsyncWebServerRequest* request) {
        request->send(404, "text/plain", String("404: ") + request->methodToString() + " " + request->url() + " not found");
    });

    server.begin();
    if (server.port() == 0) {
        std::fprintf(stderr, "could not listen on port %u\n", port);
        return 1;
    }
    std::printf("web server on http://127.0.0.1:%u/  (Ctrl+C to quit)\n", server.port());

    // ---- main loop: pump the network, push a tick every second ----
    long lastTick = -1;
    for (;;) {
        AsyncEmu::pollFor(50);
        long now = static_cast<long>(uptimeSeconds());
        if (now != lastTick) {
            lastTick = now;
            events.send((String("{\"tick\":") + now + ",\"uptime\":" + now + "}").c_str(), "tick", static_cast<uint32_t>(now) + 1);
            ws.cleanupClients(16);
        }
    }
}
