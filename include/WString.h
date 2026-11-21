// Arduino-style String for the desktop emulation.
//
// A thin subclass of std::string that adds the Arduino String API (toInt,
// startsWith, indexOf, substring, ...), so code written for the ESP compiles
// unchanged. It converts implicitly to and from std::string.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PSTR
#define PSTR(s) (s)
#endif
#ifndef F
#define F(s) (s)
#endif
#ifndef FPSTR
#define FPSTR(s) (s)
#endif

class String : public std::string {
public:
    String() = default;
    String(const char* s) : std::string(s ? s : "") {}
    String(const char* s, size_t n) : std::string(s ? s : "", s ? n : 0) {}
    String(const std::string& s) : std::string(s) {}
    String(std::string&& s) : std::string(std::move(s)) {}
    String(char c) : std::string(1, c) {}
    explicit String(int v, unsigned char base = 10) : std::string(num(static_cast<long long>(v), base)) {}
    explicit String(unsigned v, unsigned char base = 10) : std::string(unum(v, base)) {}
    explicit String(long v, unsigned char base = 10) : std::string(num(v, base)) {}
    explicit String(unsigned long v, unsigned char base = 10) : std::string(unum(v, base)) {}
    explicit String(long long v, unsigned char base = 10) : std::string(num(v, base)) {}
    explicit String(unsigned long long v, unsigned char base = 10) : std::string(unum(v, base)) {}
    explicit String(float v, unsigned char decimals = 2) : std::string(flt(v, decimals)) {}
    explicit String(double v, unsigned char decimals = 2) : std::string(flt(v, decimals)) {}

    // ---- Arduino API -----------------------------------------------------
    bool concat(const String& s) { append(s); return true; }
    bool concat(const char* s) { if (s) append(s); return true; }
    bool concat(char c) { push_back(c); return true; }
    String& operator+=(const String& s) { append(s); return *this; }
    String& operator+=(const char* s) { if (s) append(s); return *this; }
    String& operator+=(char c) { push_back(c); return *this; }
    template <class T, class = typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, char>::value>::type>
    String& operator+=(T v);
    template <class T, class = typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, char>::value>::type>
    bool concat(T v) { *this += v; return true; }

    bool startsWith(const String& p) const { return size() >= p.size() && compare(0, p.size(), p) == 0; }
    bool startsWith(const String& p, size_t offset) const {
        return offset <= size() && size() - offset >= p.size() && compare(offset, p.size(), p) == 0;
    }
    bool endsWith(const String& s) const { return size() >= s.size() && compare(size() - s.size(), s.size(), s) == 0; }

    int indexOf(char c, size_t from = 0) const { size_t p = find(c, from); return p == npos ? -1 : static_cast<int>(p); }
    int indexOf(const String& s, size_t from = 0) const { size_t p = find(s, from); return p == npos ? -1 : static_cast<int>(p); }
    int lastIndexOf(char c) const { size_t p = rfind(c); return p == npos ? -1 : static_cast<int>(p); }
    int lastIndexOf(const String& s) const { size_t p = rfind(s); return p == npos ? -1 : static_cast<int>(p); }

    String substring(size_t from) const { return from >= size() ? String() : String(substr(from)); }
    String substring(size_t from, size_t to) const {
        if (from > to) std::swap(from, to);
        if (from >= size()) return String();
        return String(substr(from, to - from));
    }

    char charAt(size_t i) const { return i < size() ? (*this)[i] : 0; }
    void setCharAt(size_t i, char c) { if (i < size()) (*this)[i] = c; }

    bool equals(const String& s) const { return *this == static_cast<const std::string&>(s); }
    bool equals(const char* s) const { return s && std::string::compare(s) == 0; }
    bool equalsIgnoreCase(const String& s) const {
        if (size() != s.size()) return false;
        for (size_t i = 0; i < size(); ++i)
            if (std::tolower(static_cast<unsigned char>((*this)[i])) != std::tolower(static_cast<unsigned char>(s[i]))) return false;
        return true;
    }

    void toLowerCase() { for (auto& c : *this) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    void toUpperCase() { for (auto& c : *this) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    void trim() {
        size_t b = 0, e = size();
        while (b < e && std::isspace(static_cast<unsigned char>((*this)[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>((*this)[e - 1]))) --e;
        assign(substr(b, e - b));
    }

    void replace(const String& find, const String& with) {
        if (find.empty()) return;
        size_t pos = 0;
        while ((pos = this->find(find, pos)) != npos) { std::string::replace(pos, find.size(), with); pos += with.size(); }
    }
    void replace(char find, char with) { for (auto& c : *this) if (c == find) c = with; }
    void remove(size_t index) { if (index < size()) erase(index); }
    void remove(size_t index, size_t count) { if (index < size()) erase(index, count); }

    long toInt() const { return std::strtol(c_str(), nullptr, 10); }
    float toFloat() const { return std::strtof(c_str(), nullptr); }
    double toDouble() const { return std::strtod(c_str(), nullptr); }
    bool reserve(size_t n) { std::string::reserve(n); return true; }

private:
    static std::string num(long long v, unsigned char base) {
        if (base == 10) return std::to_string(v);
        return v < 0 ? "-" + unum(static_cast<unsigned long long>(-v), base) : unum(static_cast<unsigned long long>(v), base);
    }
    static std::string unum(unsigned long long v, unsigned char base) {
        if (base < 2 || base > 36) base = 10;
        if (v == 0) return "0";
        std::string r;
        while (v) { r.push_back("0123456789abcdefghijklmnopqrstuvwxyz"[v % base]); v /= base; }
        std::reverse(r.begin(), r.end());
        return r;
    }
    static std::string flt(double v, unsigned char decimals) {
        char b[64];
        std::snprintf(b, sizeof b, "%.*f", decimals, v);
        return b;
    }
};

// Numbers append their decimal text, like the Arduino String (not as a raw char).
namespace detail {
template <class T>
using IsNumber = std::integral_constant<bool, std::is_arithmetic<T>::value && !std::is_same<T, char>::value>;
template <class T>
String numToString(T v, std::true_type /*floating*/) { return String(static_cast<double>(v), 2); }
template <class T>
String numToString(T v, std::false_type) {
    return std::is_signed<T>::value ? String(static_cast<long long>(v)) : String(static_cast<unsigned long long>(v));
}
}  // namespace detail

template <class T, class = typename std::enable_if<detail::IsNumber<T>::value>::type>
String operator+(const String& a, T v) { return a + detail::numToString(v, std::is_floating_point<T>()); }
template <class T, class = typename std::enable_if<detail::IsNumber<T>::value>::type>
String operator+(T v, const String& a) { return detail::numToString(v, std::is_floating_point<T>()) + a; }

inline String operator+(const String& a, const String& b) { String r(a); r.append(b); return r; }
inline String operator+(const String& a, const char* b) { String r(a); if (b) r.append(b); return r; }
inline String operator+(const char* a, const String& b) { String r(a); r.append(b); return r; }
inline String operator+(const String& a, const std::string& b) { String r(a); r.append(b); return r; }
inline String operator+(const std::string& a, const String& b) { String r(a); r.append(b); return r; }
inline String operator+(const String& a, std::string&& b) { String r(a); r.append(b); return r; }
inline String operator+(std::string&& a, const String& b) { String r(std::move(a)); r.append(b); return r; }
inline String operator+(const String& a, char b) { String r(a); r.push_back(b); return r; }

template <class T, class>
String& String::operator+=(T v) {
    append(detail::numToString(v, std::is_floating_point<T>()));
    return *this;
}
