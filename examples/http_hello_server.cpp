// Minimal HTTP/1.0 server built directly on AsyncServer (no web server layer yet).
// Usage: http_hello_server [port]   (default 8080)      Try: curl -i http://127.0.0.1:8080/
#include <cstdio>
#include <cstdlib>
#include <string>

#include "ESPAsyncTCP.h"

struct Session { std::string request; };

static void respond(AsyncClient* c, const std::string& path) {
    std::string body = path == "/" ? "Hello from ESPAsyncEmu!\n" : "Not found: " + path + "\n";
    std::string head = std::string(path == "/" ? "HTTP/1.0 200 OK" : "HTTP/1.0 404 Not Found") +
                       "\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\nConnection: close\r\n\r\n";
    std::string all = head + body;
    // A real server would honour space() and continue from onAck for big bodies.
    c->write(all.data(), all.size());
    c->close();  // graceful: flushes the response first
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // show log lines immediately, even when piped
    uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 8080;

    AsyncServer server(port);
    server.onClient([](void*, AsyncClient* c) {
        auto* s = new Session();
        c->onData([](void* arg, AsyncClient* cl, void* data, size_t len) {
            auto* sess = static_cast<Session*>(arg);
            sess->request.append(static_cast<char*>(data), len);
            size_t eol = sess->request.find("\r\n");
            if (eol == std::string::npos || sess->request.find("\r\n\r\n") == std::string::npos) return;
            // "GET /path HTTP/1.1"
            size_t a = sess->request.find(' '), b = sess->request.find(' ', a + 1);
            std::string path = (a != std::string::npos && b != std::string::npos) ? sess->request.substr(a + 1, b - a - 1) : "/";
            std::printf("%s\n", sess->request.substr(0, eol).c_str());
            respond(cl, path);
        }, s);
        c->onDisconnect([](void* arg, AsyncClient* cl) {
            delete static_cast<Session*>(arg);
            delete cl;
        }, s);
        c->setRxTimeout(10);
    });
    server.begin();
    if (server.status() == 0) {
        std::fprintf(stderr, "could not listen on port %u\n", port);
        return 1;
    }
    std::printf("HTTP server on http://127.0.0.1:%u/ (Ctrl+C to quit)\n", server.port());
    for (;;) AsyncEmu::pollFor(100);
}
