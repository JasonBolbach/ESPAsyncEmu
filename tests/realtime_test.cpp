// Tests for AsyncWebSocket and AsyncEventSource.
#include <cstring>
#include <memory>

#include "ESPAsyncWebServer.h"
#include "WebUtil.h"
#include "http_test_util.h"
#include "ws_test_util.h"

struct Ev {
    AwsEventType type;
    uint32_t id;
    uint8_t opcode = 0, msgop = 0, fin = 0;
    uint64_t index = 0, flen = 0;
    uint32_t num = 0;
    std::string data;
};

// ---------------------------------------------------------------------------
int main() {
    AsyncWebServer server(0);
    AsyncWebSocket ws("/ws");
    AsyncWebSocket wsAuth("/wsauth");
    AsyncEventSource events("/events");
    AsyncEventSource eventsAuth("/events-auth");

    std::vector<Ev> evs;
    ws.onEvent([&](AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType t, void* arg, uint8_t* data, size_t len) {
        Ev e;
        e.type = t;
        e.id = c->id();
        if (t == WS_EVT_CONNECT) EXPECT(arg != nullptr);  // the upgrade request
        if (t == WS_EVT_DATA) {
            auto* i = static_cast<AwsFrameInfo*>(arg);
            e.opcode = i->opcode; e.msgop = i->message_opcode; e.fin = i->final; e.index = i->index; e.flen = i->len; e.num = i->num;
            e.data.assign(reinterpret_cast<char*>(data), len);
            bool whole = i->final && i->index == 0 && i->len == len && i->opcode == WS_TEXT;
            if (whole && e.data.compare(0, 5, "echo ") == 0) c->text(String("echo:") + e.data.substr(5));
            if (whole && e.data == "bcast") s->textAll("broadcast!");
            if (whole && e.data == "printf") s->printfAll("n=%d", 7);
            if (whole && e.data == "close-me") c->close(1001, "going away");
            if (i->opcode == WS_BINARY && i->final && i->index + len == i->len) c->binary("bin-ack", 7);
        }
        if (t == WS_EVT_PONG) e.data.assign(reinterpret_cast<char*>(data), len);
        evs.push_back(e);
    });
    wsAuth.setAuthentication("u", "p");
    wsAuth.onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType, void*, uint8_t*, size_t) {});

    AsyncEventSourceClient* lastSse = nullptr;
    int sseConnects = 0;
    events.onConnect([&](AsyncEventSourceClient* c) { ++sseConnects; lastSse = c; });
    eventsAuth.authorizeConnect([](AsyncWebServerRequest* r) { return r->hasArg("token"); });

    server.addHandler(&ws);
    server.addHandler(&wsAuth);
    server.addHandler(&events);
    server.addHandler(&eventsAuth);
    server.begin();
    uint16_t port = server.port();

    // ===================== WebSocket: handshake =====================
    {
        WsTestClient a;
        CHECK(a.handshake(port));
        CHECK(a.upgraded);
        CHECK(a.head.find("Upgrade: websocket") != std::string::npos);
        CHECK(a.head.find("Connection: Upgrade") != std::string::npos);
        // RFC 6455 section 1.3 example key -> accept value
        CHECK(a.head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
        CHECK(pumpUntil([&] { return ws.count() == 1; }));
        CHECK(!evs.empty() && evs[0].type == WS_EVT_CONNECT);
        uint32_t id = evs[0].id;
        CHECK(ws.hasClient(id));
        CHECK(ws.client(id)->status() == WS_CONNECTED);
        CHECK(ws.client(id)->remoteIP() == IPAddress(127, 0, 0, 1));

        // ---- text echo ----
        evs.clear();
        a.send(WS_TEXT, "echo hello");
        CHECK(a.waitFrames(1));
        CHECK(a.frames[0].opcode == WS_TEXT && a.frames[0].fin);
        CHECK(a.frames[0].payload == "echo:hello");
        CHECK(evs.size() == 1 && evs[0].type == WS_EVT_DATA);
        CHECK(evs[0].opcode == WS_TEXT && evs[0].msgop == WS_TEXT && evs[0].fin == 1);
        CHECK(evs[0].index == 0 && evs[0].flen == 10 && evs[0].num == 0);
        a.frames.clear();

        // ---- empty text frame still produces a DATA event ----
        evs.clear();
        a.send(WS_TEXT, "");
        CHECK(pumpUntil([&] { return !evs.empty(); }));
        CHECK(evs[0].type == WS_EVT_DATA && evs[0].flen == 0 && evs[0].data.empty());

        // ---- fragmented message: TEXT(fin=0) + CONTINUATION(fin=1) ----
        evs.clear();
        a.send(WS_TEXT, "Hel", false);
        a.send(WS_CONTINUATION, "lo", true);
        CHECK(pumpUntil([&] { return evs.size() >= 2; }));
        CHECK(evs[0].opcode == WS_TEXT && evs[0].fin == 0 && evs[0].num == 0 && evs[0].data == "Hel");
        CHECK(evs[1].opcode == WS_CONTINUATION && evs[1].msgop == WS_TEXT && evs[1].fin == 1 && evs[1].num == 1 && evs[1].data == "lo");

        // ---- large binary frame (64-bit length), delivered in chunks, and a large reply ----
        evs.clear();
        std::string big = pseudoRandomBytes(70000, 4242);
        a.send(WS_BINARY, big);
        CHECK(a.waitFrames(1, 5000));
        CHECK(a.frames[0].opcode == WS_BINARY && a.frames[0].payload == "bin-ack");
        std::string got;
        size_t chunks = 0;
        bool indexOk = true;
        for (auto& e : evs)
            if (e.type == WS_EVT_DATA) {
                indexOk = indexOk && e.index == got.size() && e.flen == 70000;
                got += e.data;
                ++chunks;
            }
        CHECK(got == big);
        CHECK(indexOk);
        CHECK(chunks > 1);
        a.frames.clear();

        // server -> client large binary (extended length encoding on the way out)
        ws.binary(id, big.data(), big.size());
        CHECK(a.waitFrames(1, 5000));
        CHECK(a.frames[0].opcode == WS_BINARY && a.frames[0].payload == big);
        a.frames.clear();

        // ---- ping -> automatic pong (payload echoed) ----
        a.send(WS_PING, "pp");
        CHECK(a.waitFrames(1));
        CHECK(a.frames[0].opcode == WS_PONG && a.frames[0].payload == "pp");
        a.frames.clear();

        // ---- server ping -> client pong -> WS_EVT_PONG ----
        evs.clear();
        ws.pingAll();
        CHECK(a.waitFrames(1));
        CHECK(a.frames[0].opcode == WS_PING);
        a.send(WS_PONG, a.frames[0].payload);
        CHECK(pumpUntil([&] { return !evs.empty(); }));
        CHECK(evs[0].type == WS_EVT_PONG);
        a.frames.clear();

        // ---- broadcast to several clients ----
        WsTestClient b;
        CHECK(b.handshake(port));
        CHECK(pumpUntil([&] { return ws.count() == 2; }));
        a.send(WS_TEXT, "bcast");
        CHECK(a.waitFrames(1));
        CHECK(b.waitFrames(1));
        CHECK(a.frames[0].payload == "broadcast!" && b.frames[0].payload == "broadcast!");
        a.send(WS_TEXT, "printf");
        CHECK(pumpUntil([&] { return a.frames.size() >= 2 && b.frames.size() >= 2; }));
        CHECK(a.frames[1].payload == "n=7" && b.frames[1].payload == "n=7");
        ws.textAll(ws.makeBuffer(reinterpret_cast<const uint8_t*>("buf"), 3));
        CHECK(pumpUntil([&] { return a.frames.size() >= 3 && b.frames.size() >= 3; }));
        CHECK(a.frames[2].payload == "buf");
        a.frames.clear();
        b.frames.clear();

        // ---- client-initiated close handshake ----
        evs.clear();
        std::string closePayload = std::string("\x03\xE8", 2) + "bye";  // 1000 "bye"
        b.send(WS_DISCONNECT, closePayload);
        CHECK(b.waitFrames(1));
        CHECK(b.frames[0].opcode == WS_DISCONNECT);
        CHECK(b.frames[0].payload.size() >= 2 && static_cast<uint8_t>(b.frames[0].payload[0]) == 0x03 && static_cast<uint8_t>(b.frames[0].payload[1]) == 0xE8);
        CHECK(pumpUntil([&] { return b.closed; }));
        CHECK(pumpUntil([&] { return ws.count() == 1; }));
        bool sawDisc = false;
        for (auto& e : evs) sawDisc = sawDisc || e.type == WS_EVT_DISCONNECT;
        CHECK(sawDisc);

        // ---- server-initiated close: client replies and the TCP connection ends ----
        a.send(WS_TEXT, "close-me");
        CHECK(a.waitFrames(1));
        CHECK(a.frames[0].opcode == WS_DISCONNECT);
        CHECK(static_cast<uint8_t>(a.frames[0].payload[0]) == 0x03 && static_cast<uint8_t>(a.frames[0].payload[1]) == 0xE9);  // 1001
        CHECK(a.frames[0].payload.substr(2) == "going away");
        a.send(WS_DISCONNECT, a.frames[0].payload.substr(0, 2));
        CHECK(pumpUntil([&] { return a.closed; }));
        CHECK(pumpUntil([&] { return ws.count() == 0; }));
    }

    // ===================== WebSocket: bad handshakes =====================
    {
        WsTestClient a;
        CHECK(a.handshake(port, "/ws", "", "dGhlIHNhbXBsZSBub25jZQ==", "8"));
        CHECK(!a.upgraded);
        CHECK(a.head.find("HTTP/1.1 400") == 0);
        CHECK(a.head.find("Sec-WebSocket-Version: 13") != std::string::npos);

        // a plain (non-upgrade) GET to the websocket URL isn't handled by it
        HttpResponse r = httpGet(port, "/ws");
        CHECK(r.status == 404);

        // authentication
        WsTestClient u;
        CHECK(u.handshake(port, "/wsauth"));
        CHECK(!u.upgraded);
        CHECK(u.head.find("HTTP/1.1 401") == 0);
        WsTestClient v;
        CHECK(v.handshake(port, "/wsauth", "Authorization: Basic " + AsyncEmu::web::base64Encode("u:p") + "\r\n"));
        CHECK(v.upgraded);
    }

    // ===================== WebSocket: queue limit, keep-alive, cleanup =====================
    {
        evs.clear();
        WsTestClient a;
        CHECK(a.handshake(port));
        CHECK(pumpUntil([&] { return ws.count() >= 1; }));
        uint32_t id = 0;
        for (auto& e : evs) if (e.type == WS_EVT_CONNECT) id = e.id;
        AsyncWebSocketClient* wc = ws.client(id);
        CHECK(wc != nullptr);

        // Flood without pumping: the queue is bounded, extra messages are dropped.
        std::string chunk(4000, 'q');
        for (int i = 0; i < 200; ++i) wc->text(chunk.c_str(), chunk.size());
        CHECK(wc->queueLen() <= WS_MAX_QUEUED_MESSAGES);
        CHECK(wc->queueIsFull());
        CHECK(!wc->canSend());
        CHECK(pumpUntil([&] { return wc->queueLen() == 0; }, 5000));  // drains as the client reads

        // keep-alive pings
        a.frames.clear();
        wc->keepAlivePeriod(1);
        CHECK(pumpUntil([&] {
            for (auto& f : a.frames) if (f.opcode == WS_PING) return true;
            return false;
        }, 3000));
        wc->keepAlivePeriod(0);

        // cleanupClients closes the oldest connection when over the limit
        a.frames.clear();
        ws.cleanupClients(0);
        CHECK(pumpUntil([&] {
            for (auto& f : a.frames) if (f.opcode == WS_DISCONNECT) return true;
            return false;
        }));
        a.send(WS_DISCONNECT, "");
        CHECK(pumpUntil([&] { return a.closed; }));
        CHECK(pumpUntil([&] { return ws.count() == 0; }));
    }

    // ===================== Server-Sent Events =====================
    {
        AsyncClient c;
        std::string raw;
        bool up = false, closed = false;
        c.onConnect([&](void*, AsyncClient*) { up = true; });
        c.onData([&](void*, AsyncClient*, void* d, size_t n) { raw.append(static_cast<char*>(d), n); });
        c.onDisconnect([&](void*, AsyncClient*) { closed = true; });
        c.connect(IPAddress(127, 0, 0, 1), port);
        CHECK(pumpUntil([&] { return up; }));
        std::string req = "GET /events HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\nLast-Event-ID: 41\r\n\r\n";
        c.write(req.data(), req.size());
        CHECK(pumpUntil([&] { return raw.find("\r\n\r\n") != std::string::npos && sseConnects == 1; }));
        CHECK(raw.find("HTTP/1.1 200 OK") == 0);
        CHECK(raw.find("Content-Type: text/event-stream") != std::string::npos);
        CHECK(raw.find("Cache-Control: no-cache") != std::string::npos);
        CHECK(raw.find("Connection: keep-alive") != std::string::npos);
        CHECK(raw.find("Content-Length") == std::string::npos);
        CHECK(events.count() == 1);
        CHECK(lastSse && lastSse->lastId() == 41);
        CHECK(lastSse->connected());

        raw.clear();
        events.send("plain message");
        CHECK(pumpUntil([&] { return raw == "data: plain message\r\n\r\n"; }));

        raw.clear();
        events.send("msg", "update", 5, 3000);
        CHECK(pumpUntil([&] { return raw == "retry: 3000\r\nid: 5\r\nevent: update\r\ndata: msg\r\n\r\n"; }));

        raw.clear();
        events.send("line1\nline2\r\nline3");
        CHECK(pumpUntil([&] { return raw == "data: line1\r\ndata: line2\r\ndata: line3\r\n\r\n"; }));

        raw.clear();
        lastSse->send("to this client", "direct");
        CHECK(pumpUntil([&] { return raw == "event: direct\r\ndata: to this client\r\n\r\n"; }));

        // lots of data: queued and drained as the send buffer allows
        raw.clear();
        std::string payload(3000, 'x');
        for (int i = 0; i < 20; ++i) events.send(payload.c_str());
        CHECK(events.avgPacketsWaiting() <= SSE_MAX_QUEUED_MESSAGES);
        CHECK(pumpUntil([&] { return raw.size() >= 20 * (6 + 3000 + 4) || (events.avgPacketsWaiting() == 0 && raw.size() > 3000); }, 5000));

        // client leaves
        c.close();
        CHECK(pumpUntil([&] { return events.count() == 0; }));
    }

    {
        // authorizeConnect rejects; no Accept header means it's an ordinary GET
        HttpResponse r = httpGet(port, "/events-auth", "Accept: text/event-stream\r\n");
        CHECK(r.status == 401);
        HttpResponse plain = httpGet(port, "/events");
        CHECK(plain.status == 404);
    }

    // ===================== teardown with live clients =====================
    {
        auto* s2 = new AsyncWebServer(0);
        auto* ws2 = new AsyncWebSocket("/ws");
        auto* ev2 = new AsyncEventSource("/ev");
        int disconnects = 0;
        ws2->onEvent([&](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType t, void*, uint8_t*, size_t) {
            if (t == WS_EVT_DISCONNECT) ++disconnects;
        });
        s2->addHandler(ws2);
        s2->addHandler(ev2);
        s2->begin();

        WsTestClient w;
        CHECK(w.handshake(s2->port()));
        CHECK(pumpUntil([&] { return ws2->count() == 1; }));

        AsyncClient sse;
        bool up = false;
        sse.onConnect([&](void*, AsyncClient*) { up = true; });
        sse.connect(IPAddress(127, 0, 0, 1), s2->port());
        CHECK(pumpUntil([&] { return up; }));
        std::string req = "GET /ev HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n";
        sse.write(req.data(), req.size());
        CHECK(pumpUntil([&] { return ev2->count() == 1; }));

        delete ws2;  // closes its connection, no event handler callbacks
        delete ev2;
        delete s2;   // handlers are not owned by the server
        CHECK(disconnects == 0);
        CHECK(pumpUntil([&] { return w.closed; }));
    }

    CHECK(!testFailed());
    std::puts("realtime_test OK");
    return 0;
}
