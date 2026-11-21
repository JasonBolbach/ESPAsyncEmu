#include "WebUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>

namespace AsyncEmu {
namespace web {

// ---------------------------------------------------------------------------
String urlDecode(const std::string& s, bool plusAsSpace) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+' && plusAsSpace) {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < s.size() + 0 && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return String(out);
}

// ---------------------------------------------------------------------------
static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string& in) {
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        out.push_back(kB64[v >> 18]); out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]); out.push_back(kB64[v & 63]);
        i += 3;
    }
    if (i + 1 == in.size()) {
        uint32_t v = uint8_t(in[i]) << 16;
        out.push_back(kB64[v >> 18]); out.push_back(kB64[(v >> 12) & 63]); out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
        out.push_back(kB64[v >> 18]); out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]); out.push_back('=');
    }
    return out;
}

bool base64Decode(const std::string& in, std::string& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        const char* p = std::strchr(kB64, c);
        if (!p || !c) return false;
        acc = (acc << 6) | static_cast<uint32_t>(p - kB64);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<char>((acc >> bits) & 0xFF)); }
    }
    return true;
}

// ---------------------------------------------------------------------------
// MD5 (RFC 1321)
namespace {
struct Md5 {
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;

    static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | (uint32_t(p[i * 4 + 3]) << 24);
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; ++i) {
            uint32_t F;
            int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { F = C ^ (B | ~D); g = (7 * i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + rol(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
};
}  // namespace

std::string md5Hex(const std::string& in) {
    Md5 m;
    std::string msg = in;
    uint64_t bitLen = static_cast<uint64_t>(in.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 0; i < 8; ++i) msg.push_back(static_cast<char>((bitLen >> (8 * i)) & 0xFF));
    for (size_t i = 0; i < msg.size(); i += 64) m.block(reinterpret_cast<const uint8_t*>(msg.data()) + i);
    char out[33];
    uint32_t w[4] = {m.a0, m.b0, m.c0, m.d0};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) std::snprintf(out + i * 8 + j * 2, 3, "%02x", (w[i] >> (8 * j)) & 0xFF);
    return std::string(out, 32);
}

// ---------------------------------------------------------------------------
// SHA-1 (RFC 3174)
std::string sha1Raw(const std::string& in) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string msg = in;
    uint64_t bitLen = static_cast<uint64_t>(in.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bitLen >> (8 * i)) & 0xFF));
    auto rol = [](uint32_t x, int c) { return (x << c) | (x >> (32 - c)); };
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        const uint8_t* p = reinterpret_cast<const uint8_t*>(msg.data()) + off;
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t(p[i * 4]) << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::string out(20, 0);
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<char>((h[i] >> (24 - 8 * j)) & 0xFF);
    return out;
}

// ---------------------------------------------------------------------------
String mimeTypeForPath(const std::string& path) {
    static const std::map<std::string, const char*> kTypes = {
        {"htm", "text/html"}, {"html", "text/html"}, {"css", "text/css"}, {"js", "application/javascript"},
        {"mjs", "application/javascript"}, {"json", "application/json"}, {"xml", "text/xml"},
        {"txt", "text/plain"}, {"csv", "text/csv"}, {"png", "image/png"}, {"gif", "image/gif"},
        {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"ico", "image/x-icon"}, {"svg", "image/svg+xml"},
        {"webp", "image/webp"}, {"pdf", "application/pdf"}, {"zip", "application/zip"},
        {"gz", "application/x-gzip"}, {"woff", "font/woff"}, {"woff2", "font/woff2"},
        {"ttf", "font/ttf"}, {"wasm", "application/wasm"}, {"mp3", "audio/mpeg"}, {"mp4", "video/mp4"}};
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "text/plain";
    auto it = kTypes.find(toLower(path.substr(dot + 1)));
    return it == kTypes.end() ? "text/plain" : it->second;
}

const char* statusText(int code) {
    switch (code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 305: return "Use Proxy";
        case 307: return "Temporary Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 402: return "Payment Required";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 407: return "Proxy Authentication Required";
        case 408: return "Request Time-out";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Request Entity Too Large";
        case 414: return "Request-URI Too Large";
        case 415: return "Unsupported Media Type";
        case 416: return "Requested range not satisfiable";
        case 417: return "Expectation Failed";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Time-out";
        case 505: return "HTTP Version not supported";
        default: return "";
    }
}

std::string httpDate(time_t t) {
    char buf[64];
    std::tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &tmv);
    return buf;
}

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string randomHex32() {
    static std::mt19937_64 rng{std::random_device{}()};
    char buf[33];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(rng()), static_cast<unsigned long long>(rng()));
    return buf;
}

}  // namespace web
}  // namespace AsyncEmu
