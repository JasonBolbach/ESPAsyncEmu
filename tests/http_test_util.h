// Tiny HTTP/1.x test client built on the emulated AsyncClient.
#pragma once
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "ESPAsyncTCP.h"
#include "test_util.h"

struct HttpResponse {
    bool completed = false;  // the server closed the connection (or we got a full response)
    int status = 0;
    std::string statusLine, rawHead, body, raw;
    std::map<std::string, std::string> headers;  // lower-case names
    bool chunked = false;

    std::string header(const std::string& name) const {
        std::string k = name;
        for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto it = headers.find(k);
        return it == headers.end() ? std::string() : it->second;
    }
    bool has(const std::string& name) const {
        std::string k = name;
        for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return headers.count(k) > 0;
    }
};

inline std::string decodeChunked(const std::string& in, bool* ok = nullptr) {
    std::string out;
    size_t i = 0;
    if (ok) *ok = false;
    while (i < in.size()) {
        size_t eol = in.find("\r\n", i);
        if (eol == std::string::npos) return out;
        size_t len = std::stoul(in.substr(i, eol - i), nullptr, 16);
        i = eol + 2;
        if (len == 0) { if (ok) *ok = true; return out; }
        if (i + len + 2 > in.size()) return out;
        out.append(in, i, len);
        i += len + 2;
    }
    return out;
}

inline HttpResponse parseHttp(const std::string& raw) {
    HttpResponse r;
    r.raw = raw;
    size_t end = raw.find("\r\n\r\n");
    if (end == std::string::npos) return r;
    r.rawHead = raw.substr(0, end);
    std::string body = raw.substr(end + 4);
    size_t pos = 0, eol = r.rawHead.find("\r\n");
    r.statusLine = r.rawHead.substr(0, eol);
    size_t sp = r.statusLine.find(' ');
    if (sp != std::string::npos) r.status = std::atoi(r.statusLine.c_str() + sp + 1);
    pos = eol == std::string::npos ? r.rawHead.size() : eol + 2;
    while (pos < r.rawHead.size()) {
        size_t e = r.rawHead.find("\r\n", pos);
        if (e == std::string::npos) e = r.rawHead.size();
        std::string line = r.rawHead.substr(pos, e - pos);
        pos = e + 2;
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c), v = line.substr(c + 1);
        for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        r.headers[k] = v;
    }
    r.chunked = r.header("transfer-encoding") == "chunked";
    r.body = r.chunked ? decodeChunked(body) : body;
    return r;
}

// Sends the pieces in order (pumping `pumpBetweenMs` after each so they tend to
// arrive separately), then waits for the server to close the connection.
inline HttpResponse httpExchange(uint16_t port, const std::vector<std::string>& pieces, unsigned timeoutMs = 4000,
                                 unsigned pumpBetweenMs = 0) {
    AsyncClient c;
    std::string raw;
    bool connected = false, done = false;
    c.setNoDelay(true);
    c.onConnect([&](void*, AsyncClient*) { connected = true; });
    c.onData([&](void*, AsyncClient*, void* d, size_t n) { raw.append(static_cast<char*>(d), n); });
    c.onDisconnect([&](void*, AsyncClient*) { done = true; });
    c.onError([&](void*, AsyncClient*, int8_t) { done = true; });
    c.connect(IPAddress(127, 0, 0, 1), port);

    size_t pi = 0, off = 0;
    bool finished = pumpUntil(
        [&] {
            if (connected && pi < pieces.size() && c.connected()) {
                const std::string& pc = pieces[pi];
                size_t n = c.write(pc.data() + off, std::min(c.space(), pc.size() - off));
                off += n;
                if (off == pc.size()) {
                    ++pi;
                    off = 0;
                    if (pumpBetweenMs) AsyncEmu::pollFor(pumpBetweenMs);
                }
            }
            return done;
        },
        timeoutMs);
    HttpResponse r = parseHttp(raw);
    r.completed = finished;
    r.raw = raw;
    return r;
}

inline std::string httpReq(const std::string& method, const std::string& path, const std::string& headers = "",
                           const std::string& body = "", const char* ver = "HTTP/1.1") {
    std::string r = method + " " + path + " " + ver + "\r\nHost: localhost\r\n" + headers;
    if (!body.empty()) r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "\r\n";
    r += body;
    return r;
}

inline HttpResponse httpGet(uint16_t port, const std::string& path, const std::string& headers = "", const char* ver = "HTTP/1.1") {
    return httpExchange(port, {httpReq("GET", path, headers, "", ver)});
}

inline std::string pseudoRandomBytes(size_t n, uint32_t seed = 12345) {
    std::string s(n, 0);
    for (size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        s[i] = static_cast<char>(seed >> 24);
    }
    return s;
}
