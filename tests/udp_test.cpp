#include "ESPAsyncUDP.h"
#include "test_util.h"

#include <cstring>
#include <string>

static const IPAddress kLoop(127, 0, 0, 1);

int main() {
    // ---- listen + writeTo + reply through the packet -----------------------
    AsyncUDP server;
    CHECK(!server);
    CHECK(server.listen(0));
    CHECK(server);
    uint16_t port = server.localPort();
    CHECK(port != 0);

    std::string serverGot;
    int serverPackets = 0;
    server.onPacket([&](AsyncUDPPacket& p) {
        ++serverPackets;
        serverGot.assign(reinterpret_cast<char*>(p.data()), p.length());
        EXPECT(p.remoteIP() == kLoop);
        EXPECT(p.localPort() == port);
        EXPECT(!p.isBroadcast());
        p.print("pong");  // reply to the sender
    });

    AsyncUDP client;
    std::string clientGot;
    int arg = 7, seenArg = 0;
    client.onPacket([&](void* a, AsyncUDPPacket& p) {
        seenArg = *static_cast<int*>(a);
        clientGot.assign(reinterpret_cast<char*>(p.data()), p.length());
        EXPECT(p.remotePort() == port);
    }, &arg);

    const char* ping = "ping";
    CHECK(client.writeTo(reinterpret_cast<const uint8_t*>(ping), 4, kLoop, port) == 4);
    CHECK(pumpUntil([&] { return clientGot == "pong"; }));
    CHECK(serverGot == "ping");
    CHECK(seenArg == 7);

    // ---- connect() + write() + AsyncUDPMessage ------------------------------
    AsyncUDP c2;
    CHECK(c2.write(reinterpret_cast<const uint8_t*>("x"), 1) == 0);  // not connected yet
    CHECK(c2.connect(kLoop, port));
    AsyncUDPMessage msg(8);
    CHECK(msg.print("12345678overflow") == 8);  // capped at capacity
    CHECK(msg.space() == 0);
    CHECK(c2.send(msg) == 8);
    CHECK(pumpUntil([&] { return serverGot == "12345678"; }));
    CHECK(serverPackets == 2);

    // ---- close stops delivery ------------------------------------------------
    server.close();
    CHECK(!server);
    int before = serverPackets;
    c2.write(reinterpret_cast<const uint8_t*>("late"), 4);
    for (int i = 0; i < 10; ++i) AsyncEmu::pollFor(5);
    CHECK(serverPackets == before);

    // ---- deleting from inside the callback is safe ------------------------------
    {
        AsyncUDP* s = new AsyncUDP();
        EXPECT(s->listen(0));
        uint16_t p = s->localPort();
        bool hit = false;
        s->onPacket([&](AsyncUDPPacket&) { hit = true; delete s; });
        AsyncUDP c;
        c.writeTo(reinterpret_cast<const uint8_t*>("z"), 1, kLoop, p);
        EXPECT(pumpUntil([&] { return hit; }));
        for (int i = 0; i < 10; ++i) AsyncEmu::pollFor(5);
    }

    CHECK(!testFailed());
    std::puts("udp_test OK");
    return 0;
}
