// Self-contained TCP demo: a server and a client in one process talking over
// loopback, driven by a single poll loop. Exits 0 on success (used by ctest).
#include <chrono>
#include <cstdio>
#include <string>

#include "ESPAsyncTCP.h"

int main() {
    AsyncServer server(0);  // 0 = pick a free port
    server.onClient([](void*, AsyncClient* c) {
        c->onData([](void*, AsyncClient* cl, void* data, size_t len) {
            std::string msg(static_cast<char*>(data), len);
            std::printf("server got: %s\n", msg.c_str());
            std::string reply = "echo: " + msg;
            cl->write(reply.data(), reply.size());
        });
        c->onDisconnect([](void*, AsyncClient* cl) { delete cl; });
    });
    server.begin();

    AsyncClient client;
    std::string reply;
    bool done = false;
    client.onConnect([](void*, AsyncClient* c) {
        std::printf("client connected to %s:%u\n", c->remoteIP().toString().c_str(), c->remotePort());
        c->write("hello");
    });
    client.onData([&](void*, AsyncClient* c, void* data, size_t len) {
        reply.assign(static_cast<char*>(data), len);
        std::printf("client got: %s\n", reply.c_str());
        c->close();
    });
    client.onDisconnect([&](void*, AsyncClient*) { done = true; });
    client.onError([&](void*, AsyncClient* c, int8_t err) {
        std::printf("client error: %s\n", c->errorToString(err));
        done = true;
    });
    client.connect(IPAddress(127, 0, 0, 1), server.port());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done && std::chrono::steady_clock::now() < deadline) AsyncEmu::pollFor(10);

    return reply == "echo: hello" ? 0 : 1;
}
