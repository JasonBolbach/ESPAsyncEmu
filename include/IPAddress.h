// Minimal Arduino-compatible IPAddress (IPv4 only) for the desktop emulation.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

class IPAddress {
public:
    IPAddress() { std::memset(_a, 0, 4); }
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) { _a[0] = a; _a[1] = b; _a[2] = c; _a[3] = d; }
    // Like Arduino: the 4 bytes are stored in memory order (a == lowest byte).
    IPAddress(uint32_t address) { std::memcpy(_a, &address, 4); }
    IPAddress(const uint8_t* p) { std::memcpy(_a, p, 4); }

    bool fromString(const char* s) {
        unsigned v[4];
        char tail;
        if (!s || std::sscanf(s, "%u.%u.%u.%u%c", &v[0], &v[1], &v[2], &v[3], &tail) != 4) return false;
        for (int i = 0; i < 4; ++i) { if (v[i] > 255) return false; }
        for (int i = 0; i < 4; ++i) _a[i] = static_cast<uint8_t>(v[i]);
        return true;
    }

    operator uint32_t() const { uint32_t v; std::memcpy(&v, _a, 4); return v; }
    bool operator==(const IPAddress& o) const { return std::memcmp(_a, o._a, 4) == 0; }
    bool operator!=(const IPAddress& o) const { return !(*this == o); }
    uint8_t operator[](int i) const { return _a[i]; }
    uint8_t& operator[](int i) { return _a[i]; }

    std::string toString() const {
        char b[16];
        std::snprintf(b, sizeof b, "%u.%u.%u.%u", _a[0], _a[1], _a[2], _a[3]);
        return b;
    }

private:
    uint8_t _a[4];
};
