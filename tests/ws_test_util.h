// WebSocket test client (client frames are masked, as RFC 6455 requires).
#pragma once
#include <cstring>
#include <string>
#include <vector>

#include "ESPAsyncTCP.h"
#include "test_util.h"

// ---------------------------------------------------------------------------
struct WsFrame {
    uint8_t opcode;
    bool fin;
    std::string payload;
};

class WsTestClient {
public:
    AsyncClient c;
    std::string raw, head;
    bool up = false, closed = false, upgraded = false;
    std::vector<WsFrame> frames;
    uint32_t seed = 99;

    WsTestClient() {
        c.setNoDelay(true);
        c.onConnect([this](void*, AsyncClient*) { up = true; });
        c.onData([this](void*, AsyncClient*, void* d, size_t n) { raw.append(static_cast<char*>(d), n); parse(); });
        c.onDisconnect([this](void*, AsyncClient*) { closed = true; });
    }

    // Returns the raw handshake response head once it arrived.
    bool handshake(uint16_t port, const std::string& path = "/ws", const std::string& extra = "",
                   const std::string& key = "dGhlIHNhbXBsZSBub25jZQ==", const std::string& version = "13") {
        c.connect(IPAddress(127, 0, 0, 1), port);
        if (!pumpUntil([&] { return up; })) return false;
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: " + version + "\r\n" + extra + "\r\n";
        c.write(req.data(), req.size());
        return pumpUntil([&] { return !head.empty() || closed; });
    }

    void parse() {
        if (head.empty()) {
            size_t e = raw.find("\r\n\r\n");
            if (e == std::string::npos) return;
            head = raw.substr(0, e + 4);
            raw.erase(0, e + 4);
            upgraded = head.compare(0, 12, "HTTP/1.1 101") == 0;
            if (!upgraded) return;
        }
        if (!upgraded) return;
        for (;;) {
            if (raw.size() < 2) return;
            uint8_t b0 = static_cast<uint8_t>(raw[0]), b1 = static_cast<uint8_t>(raw[1]);
            uint64_t len = b1 & 0x7F;
            size_t pos = 2;
            if (len == 126) { if (raw.size() < 4) return; len = (static_cast<uint8_t>(raw[2]) << 8) | static_cast<uint8_t>(raw[3]); pos = 4; }
            else if (len == 127) {
                if (raw.size() < 10) return;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint8_t>(raw[2 + i]);
                pos = 10;
            }
            if (raw.size() < pos + len) return;
            frames.push_back({static_cast<uint8_t>(b0 & 0x0F), (b0 & 0x80) != 0, raw.substr(pos, len)});
            raw.erase(0, pos + len);
        }
    }

    void send(uint8_t opcode, const std::string& payload, bool fin = true) {
        std::string f;
        f.push_back(static_cast<char>((fin ? 0x80 : 0) | opcode));
        size_t n = payload.size();
        if (n < 126) f.push_back(static_cast<char>(0x80 | n));
        else if (n <= 0xFFFF) { f.push_back(static_cast<char>(0x80 | 126)); f.push_back(static_cast<char>(n >> 8)); f.push_back(static_cast<char>(n & 0xFF)); }
        else { f.push_back(static_cast<char>(0x80 | 127)); for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xFF)); }
        uint8_t mask[4];
        for (auto& m : mask) { seed = seed * 1664525u + 1013904223u; m = static_cast<uint8_t>(seed >> 24); }
        f.append(reinterpret_cast<char*>(mask), 4);
        for (size_t i = 0; i < n; ++i) f.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
        size_t off = 0;
        pumpUntil([&] {
            off += c.write(f.data() + off, std::min(c.space(), f.size() - off));
            return off >= f.size();
        });
    }

    bool waitFrames(size_t n, unsigned ms = 3000) { return pumpUntil([&] { return frames.size() >= n; }, ms); }
};

