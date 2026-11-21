// Self-contained UDP demo: a responder and a requester in one process.
// Exits 0 after a ping/pong round trip (used by ctest).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "ESPAsyncUDP.h"

int main() {
    AsyncUDP responder;
    responder.listen(0);  // any free port
    responder.onPacket([](AsyncUDPPacket& p) {
        std::string msg(reinterpret_cast<char*>(p.data()), p.length());
        std::printf("responder got '%s' from %s:%u\n", msg.c_str(), p.remoteIP().toString().c_str(), p.remotePort());
        p.print("pong");  // replies go back to the sender
    });

    AsyncUDP requester;
    bool gotPong = false;
    requester.onPacket([&](AsyncUDPPacket& p) {
        std::printf("requester got '%.*s'\n", static_cast<int>(p.length()), reinterpret_cast<char*>(p.data()));
        gotPong = std::memcmp(p.data(), "pong", 4) == 0;
    });
    requester.writeTo(reinterpret_cast<const uint8_t*>("ping"), 4, IPAddress(127, 0, 0, 1), responder.localPort());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!gotPong && std::chrono::steady_clock::now() < deadline) AsyncEmu::pollFor(10);
    return gotPong ? 0 : 1;
}
