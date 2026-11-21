// TCP echo server. Usage: tcp_echo_server [port]   (default 7007)
// Try it:  nc 127.0.0.1 7007
#include <cstdio>
#include <cstdlib>

#include "ESPAsyncTCP.h"

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // show log lines immediately, even when piped
    uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 7007;

    AsyncServer server(port);
    server.onClient([](void*, AsyncClient* c) {
        std::printf("[+] %s:%u connected\n", c->remoteIP().toString().c_str(), c->remotePort());

        c->onData([](void*, AsyncClient* cl, void* data, size_t len) {
            cl->write(static_cast<const char*>(data), len);  // echo back
        });
        c->onDisconnect([](void*, AsyncClient* cl) {
            std::printf("[-] %s:%u disconnected\n", cl->remoteIP().toString().c_str(), cl->remotePort());
            delete cl;  // the application owns accepted clients
        });
        c->setRxTimeout(60);  // drop idle connections after a minute
    });
    server.begin();
    if (server.status() == 0) {
        std::fprintf(stderr, "could not listen on port %u\n", port);
        return 1;
    }
    std::printf("echo server listening on port %u (Ctrl+C to quit)\n", server.port());

    // pollFor() blocks until there is network activity, so this loop is cheap.
    for (;;) AsyncEmu::pollFor(100);
}
