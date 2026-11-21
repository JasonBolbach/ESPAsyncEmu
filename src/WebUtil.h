// Internal helpers for the web server layer.
#pragma once
#include <ctime>
#include <string>

#include "WString.h"

namespace AsyncEmu {
namespace web {

// %XX decoding. plusAsSpace is true for query strings / form bodies.
String urlDecode(const std::string& s, bool plusAsSpace);

std::string base64Encode(const std::string& in);
bool base64Decode(const std::string& in, std::string& out);

std::string md5Hex(const std::string& in);
std::string sha1Raw(const std::string& in);  // 20 raw bytes (for the WebSocket handshake)

String mimeTypeForPath(const std::string& path);  // by extension, "text/plain" fallback
const char* statusText(int code);
std::string httpDate(time_t t);

bool iequals(const std::string& a, const std::string& b);
std::string toLower(std::string s);
std::string trim(const std::string& s);

// Generates a random 32 character hex string (digest auth nonce/opaque).
std::string randomHex32();

}  // namespace web
}  // namespace AsyncEmu
