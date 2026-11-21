#include "ESPAsyncTCP.h"
#include "test_util.h"

#include <string>
#include <thread>

static const IPAddress kLoop(127, 0, 0, 1);

int main() {
    auto mainThread = std::this_thread::get_id();

    // ---- echo server + client ------------------------------------------
    AsyncServer server(0);
    CHECK(server.status() == 0);
    int accepted = 0, serverDisc = 0;
    bool wrongThread = false;
    server.onClient([&](void*, AsyncClient* c) {
        ++accepted;
        c->onData([&](void*, AsyncClient* cl, void* data, size_t len) {
            if (std::this_thread::get_id() != mainThread) wrongThread = true;
            cl->write(static_cast<const char*>(data), len);  // echo
        });
        c->onDisconnect([&](void*, AsyncClient* cl) { ++serverDisc; delete cl; });  // delete inside callback
    });
    server.begin();
    CHECK(server.status() == 1);
    CHECK(server.port() != 0);

    AsyncClient client;
    CHECK(client.disconnected());
    std::string received;
    bool connected = false, clientDisc = false, acked = false;
    client.onConnect([&](void*, AsyncClient* c) {
        connected = true;
        EXPECT(c->connected());
        EXPECT(c->remoteIP() == kLoop);
        EXPECT(c->remotePort() == server.port());
    });
    client.onData([&](void*, AsyncClient*, void* d, size_t n) { received.append(static_cast<char*>(d), n); });
    client.onAck([&](void*, AsyncClient*, size_t len, uint32_t) { if (len) acked = true; });
    client.onDisconnect([&](void*, AsyncClient*) { clientDisc = true; });

    CHECK(client.connect(kLoop, server.port()));
    CHECK(client.connecting());
    CHECK(!client.connect(kLoop, server.port()));  // already connecting
    CHECK(pumpUntil([&] { return connected; }));
    CHECK(accepted == 1);

    size_t before = client.space();
    CHECK(client.canSend());
    CHECK(client.write("hello ") == 6);
    CHECK(client.space() == before - 6);  // buffered until acked
    CHECK(client.add("async ", 6) == 6);
    CHECK(client.add("world", 5) == 5);
    CHECK(client.send());
    CHECK(pumpUntil([&] { return received == "hello async world"; }));
    CHECK(acked);
    CHECK(client.space() == before);      // acked -> room again
    CHECK(!wrongThread);

    // Large transfer, bounded by space(): push 100 KB in chunks.
    received.clear();
    std::string big(100000, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + i % 26);
    size_t sent = 0;
    CHECK(pumpUntil([&] {
        if (sent < big.size()) sent += client.write(big.data() + sent, std::min(client.space(), big.size() - sent));
        return received.size() == big.size();
    }, 5000));
    CHECK(received == big);

    // Graceful close: both sides see a disconnect, server client is freed.
    client.close();
    CHECK(client.disconnecting() || client.disconnected());
    CHECK(pumpUntil([&] { return clientDisc && serverDisc == 1; }));
    CHECK(client.disconnected());
    CHECK(client.free());

    // ---- reconnect with the same AsyncClient, via host name ----------------
    connected = clientDisc = false;
    received.clear();
    CHECK(client.connect("localhost", server.port()));
    CHECK(pumpUntil([&] { return connected; }));
    client.write("again");
    CHECK(pumpUntil([&] { return received == "again"; }));

    // Remote close is detected.
    server.end();
    CHECK(server.status() == 0);
    client.abort();  // RST path: onError(ERR_ABRT) then onDisconnect
    CHECK(client.disconnected());
    CHECK(pumpUntil([&] { return serverDisc == 2; }));

    // ---- connection refused -> onError then onDisconnect ----------------------
    {
        AsyncClient c;
        int8_t err = 99;
        bool disc = false;
        c.onError([&](void*, AsyncClient*, int8_t e) { err = e; });
        c.onDisconnect([&](void*, AsyncClient*) { disc = true; });
        EXPECT(c.connect(kLoop, server.port()));  // server was stopped: refused
        EXPECT(pumpUntil([&] { return disc; }));
        EXPECT(err == ERR_RST);
        EXPECT(std::string(c.errorToString(err)) == "Connection reset");
        EXPECT(c.disconnected());
    }

    // ---- rx timeout closes an idle connection ------------------------------
    {
        AsyncServer idle(0);
        AsyncClient* srvSide = nullptr;
        idle.onClient([&](void*, AsyncClient* c) { srvSide = c; });
        idle.begin();
        AsyncClient c;
        bool timedOut = false, disc = false, up = false;
        c.onConnect([&](void*, AsyncClient*) { up = true; });
        c.onTimeout([&](void*, AsyncClient*, uint32_t) { timedOut = true; });
        c.onDisconnect([&](void*, AsyncClient*) { disc = true; });
        c.setRxTimeout(1);
        c.connect(kLoop, idle.port());
        EXPECT(pumpUntil([&] { return up; }));
        EXPECT(pumpUntil([&] { return timedOut && disc; }, 3000));
        delete srvSide;
    }

    // ---- deleting a client from inside onData must be safe ----------------------
    {
        AsyncServer s2(0);
        s2.onClient([&](void*, AsyncClient* c) {
            c->onData([](void*, AsyncClient* cl, void*, size_t) { delete cl; });
        });
        s2.begin();
        AsyncClient* c = new AsyncClient();
        bool up = false;
        c->onConnect([&](void*, AsyncClient*) { up = true; });
        c->connect(kLoop, s2.port());
        EXPECT(pumpUntil([&] { return up; }));
        c->write("bye");
        for (int i = 0; i < 20; ++i) AsyncEmu::pollFor(5);  // server deletes its side; nothing crashes
        delete c;
        for (int i = 0; i < 20; ++i) AsyncEmu::pollFor(5);
    }

    CHECK(!testFailed());
    // ---- port offset for privileged ports ------------------------------------------
    {
        CHECK(AsyncEmu::portOffset() == 0);
        AsyncEmu::setPortOffset(31000);
        AsyncServer priv(80);           // would need root without the offset
        priv.begin();
        CHECK(priv.status() == 1);
        CHECK(priv.port() == 31080);
        AsyncServer high(40123);        // ports >= 1024 are never shifted
        high.begin();
        CHECK(high.port() == 40123);
        AsyncEmu::setPortOffset(0);
    }

    std::puts("tcp_test OK");
    return 0;
}
