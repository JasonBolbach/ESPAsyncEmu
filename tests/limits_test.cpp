// Device limits ("strict mode"): profiles, send buffer, connection cap, watchdog,
// strict request headers, form body limit, overflow warnings.
#include <chrono>
#include <thread>

#include "ESPAsyncWebServer.h"
#include "http_test_util.h"
#include "ws_test_util.h"

// A handler that does not register the headers it needs (the classic device-only bug).
class ForgetfulHandler : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest* r) override { return r->url() == "/forgetful"; }
    void handleRequest(AsyncWebServerRequest* r) override { r->send(200, "text/plain", "[" + r->header("X-Test") + "]"); }
};

class CarefulHandler : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest* r) override {
        if (r->url() != "/careful") return false;
        r->addInterestingHeader("X-Test");
        return true;
    }
    void handleRequest(AsyncWebServerRequest* r) override { r->send(200, "text/plain", "[" + r->header("X-Test") + "]"); }
};

static bool contains(const std::vector<std::string>& v, const std::string& needle) {
    for (auto& s : v)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    std::vector<std::string> warnings;
    AsyncEmu::setWarningHandler([&](const std::string& m) { warnings.push_back(m); });
    const AsyncEmu::DeviceLimits original = AsyncEmu::deviceLimits();

    // ===================== profiles =====================
    {
        CHECK(!AsyncEmu::setDeviceProfile("nonsense"));
        CHECK(AsyncEmu::setDeviceProfile("esp8266"));
        CHECK(AsyncEmu::deviceLimits().sendBufferSize == 1072);
        CHECK(AsyncEmu::deviceLimits().maxConnections == 5);
        CHECK(AsyncEmu::deviceLimits().strictHeaders);
        CHECK(AsyncEmu::deviceLimits().watchdogMs == 3000);
        CHECK(AsyncEmu::setDeviceProfile("esp8266-hb"));
        CHECK(AsyncEmu::deviceLimits().sendBufferSize == 2920);
        CHECK(AsyncEmu::setDeviceProfile("esp32"));
        CHECK(AsyncEmu::deviceLimits().sendBufferSize == 5744);
        CHECK(AsyncEmu::deviceLimits().maxConnections == 16);
        CHECK(AsyncEmu::setDeviceProfile("desktop"));
        CHECK(!AsyncEmu::deviceLimits().strictHeaders);
        CHECK(AsyncEmu::deviceLimits().maxConnections == 0);
        bool known = true;
        AsyncEmu::deviceProfile("bogus", &known);
        CHECK(!known);
        AsyncEmu::setDeviceLimits(original);
    }

    // ===================== send buffer size =====================
    {
        AsyncEmu::DeviceLimits l = original;
        l.sendBufferSize = 1072;
        AsyncEmu::setDeviceLimits(l);

        AsyncServer server(0);
        std::string got;
        server.onClient([&](void*, AsyncClient* c) {
            c->onData([&](void*, AsyncClient*, void* d, size_t n) { got.append(static_cast<char*>(d), n); });
            c->onDisconnect([](void*, AsyncClient* cl) { delete cl; });
        });
        server.begin();
        AsyncClient c;
        bool up = false;
        c.onConnect([&](void*, AsyncClient*) { up = true; });
        c.connect(IPAddress(127, 0, 0, 1), server.port());
        CHECK(pumpUntil([&] { return up; }));
        CHECK(c.space() == 1072);
        CHECK(c.write(std::string(5000, 'a').c_str(), 5000) == 1072);  // only what fits

        // a large transfer must still complete, just in more steps
        std::string big = pseudoRandomBytes(60000, 7);
        size_t sent = 0;
        const size_t already = 1072;  // the 'a's written above arrive first
        CHECK(pumpUntil([&] {
            if (sent < big.size()) sent += c.write(big.data() + sent, std::min(c.space(), big.size() - sent));
            return got.size() >= already + big.size();
        }, 5000));
        CHECK(got.substr(already) == big);
        AsyncEmu::setDeviceLimits(original);
    }

    // ===================== connection cap =====================
    {
        AsyncEmu::DeviceLimits l = original;
        l.maxConnections = 2;
        AsyncEmu::setDeviceLimits(l);
        warnings.clear();

        AsyncServer server(0);
        int accepted = 0;
        server.onClient([&](void*, AsyncClient* c) {
            ++accepted;
            c->onDisconnect([](void*, AsyncClient* cl) { delete cl; });
        });
        server.begin();

        AsyncClient a, b, c3;
        bool aUp = false, bUp = false, c3Closed = false, c3Up = false;
        a.onConnect([&](void*, AsyncClient*) { aUp = true; });
        b.onConnect([&](void*, AsyncClient*) { bUp = true; });
        c3.onConnect([&](void*, AsyncClient*) { c3Up = true; });
        c3.onDisconnect([&](void*, AsyncClient*) { c3Closed = true; });
        a.connect(IPAddress(127, 0, 0, 1), server.port());
        b.connect(IPAddress(127, 0, 0, 1), server.port());
        CHECK(pumpUntil([&] { return aUp && bUp && accepted == 2; }));
        c3.connect(IPAddress(127, 0, 0, 1), server.port());
        CHECK(pumpUntil([&] { return c3Closed; }));  // dropped by the "device"
        CHECK(accepted == 2);
        CHECK(contains(warnings, "maxConnections"));

        // a freed slot can be used again
        a.close();
        CHECK(pumpUntil([&] { return a.disconnected(); }));
        pumpUntil([&] { return false; }, 100);
        AsyncClient d;
        bool dUp = false, dClosed = false;
        d.onConnect([&](void*, AsyncClient*) { dUp = true; });
        d.onDisconnect([&](void*, AsyncClient*) { dClosed = true; });
        d.connect(IPAddress(127, 0, 0, 1), server.port());
        CHECK(pumpUntil([&] { return accepted == 3; }));
        CHECK(!dClosed);
        (void)c3Up;
        AsyncEmu::setDeviceLimits(original);
    }

    // ===================== watchdog and inCallback =====================
    {
        AsyncEmu::DeviceLimits l = original;
        l.watchdogMs = 50;
        AsyncEmu::setDeviceLimits(l);
        warnings.clear();

        CHECK(!AsyncEmu::inCallback());
        bool insideDuringPost = false;
        AsyncEmu::post([&] { insideDuringPost = AsyncEmu::inCallback(); });
        AsyncEmu::poll();
        CHECK(insideDuringPost);
        CHECK(!AsyncEmu::inCallback());
        CHECK(warnings.empty());  // a quick callback is fine

        AsyncEmu::post([] { std::this_thread::sleep_for(std::chrono::milliseconds(120)); });  // "delay()" in a callback
        AsyncEmu::poll();
        CHECK(contains(warnings, "watchdog"));
        CHECK(contains(warnings, "120") || contains(warnings, "12"));  // the measured time is reported

        // nested callbacks count towards the outermost one
        warnings.clear();
        AsyncEmu::post([] {
            AsyncEmu::post([] { std::this_thread::sleep_for(std::chrono::milliseconds(80)); });
            AsyncEmu::poll();  // like delay() pumping the loop from inside a callback
        });
        AsyncEmu::poll();
        CHECK(contains(warnings, "watchdog"));
        AsyncEmu::setDeviceLimits(original);
    }

    // ===================== strict request headers =====================
    {
        AsyncWebServer server(0);
        ForgetfulHandler forgetful;
        CarefulHandler careful;
        server.addHandler(&forgetful);
        server.addHandler(&careful);
        server.on("/route", [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "[" + r->header("X-Test") + "]"); });
        server.begin();
        const std::string hdr = "X-Test: hello\r\n";

        // desktop: everything is kept, so even the forgetful handler sees the header
        CHECK(httpGet(server.port(), "/forgetful", hdr).body == "[hello]");

        AsyncEmu::DeviceLimits l = original;
        l.strictHeaders = true;
        AsyncEmu::setDeviceLimits(l);
        CHECK(httpGet(server.port(), "/forgetful", hdr).body == "[]");         // not registered: dropped, as on the device
        CHECK(httpGet(server.port(), "/careful", hdr).body == "[hello]");      // registered
        CHECK(httpGet(server.port(), "/route", hdr).body == "[hello]");        // stock routes register "ANY"
        CHECK(httpGet(server.port(), "/missing", hdr).status == 404);          // catch-all too
        AsyncEmu::setDeviceLimits(original);
    }

    // ===================== form body limit =====================
    {
        AsyncWebServer server(0);
        server.on("/form", HTTP_POST, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", String(r->arg("a").length())); });
        server.begin();
        AsyncEmu::DeviceLimits l = original;
        l.maxFormBodySize = 2048;
        AsyncEmu::setDeviceLimits(l);
        const std::string ct = "Content-Type: application/x-www-form-urlencoded\r\n";
        HttpResponse r = httpExchange(server.port(), {httpReq("POST", "/form", ct, "a=" + std::string(1000, 'x'))});
        CHECK(r.status == 200 && r.body == "1000");
        r = httpExchange(server.port(), {httpReq("POST", "/form", ct, "a=" + std::string(3000, 'x'))});
        CHECK(r.status == 413);
        AsyncEmu::setDeviceLimits(original);
    }

    // ===================== queue overflow is reported =====================
    {
        AsyncWebServer server(0);
        AsyncWebSocket ws("/ws");
        AsyncWebSocketClient* wc = nullptr;
        ws.onEvent([&](AsyncWebSocket*, AsyncWebSocketClient* c, AwsEventType t, void*, uint8_t*, size_t) {
            if (t == WS_EVT_CONNECT) wc = c;
        });
        server.addHandler(&ws);
        server.begin();
        WsTestClient client;
        CHECK(client.handshake(server.port()));
        CHECK(pumpUntil([&] { return wc != nullptr; }));
        warnings.clear();
        std::string chunk(4000, 'q');
        for (int i = 0; i < 100; ++i) wc->text(chunk.c_str(), chunk.size());
        CHECK(contains(warnings, "queue full"));
        CHECK(pumpUntil([&] { return wc->queueLen() == 0; }, 5000));
    }

    AsyncEmu::setWarningHandler(nullptr);
    CHECK(!testFailed());
    std::puts("limits_test OK");
    return 0;
}
