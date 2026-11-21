// Arduino-compatible Print: anything you can print()/println()/printf() into.
// AsyncResponseStream, the emulated Serial and (via Stream) File derive from it,
// which is also what lets ArduinoJson's serializeJson(doc, destination) write to them.
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "WString.h"

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

class Print;

// Objects that know how to print themselves (ArduinoJson uses this for its own types).
class Printable {
public:
    virtual ~Printable() {}
    virtual size_t printTo(Print& p) const = 0;
};

class Print {
public:
    virtual ~Print() {}

    virtual size_t write(uint8_t c) = 0;
    virtual size_t write(const uint8_t* buffer, size_t size) {
        size_t n = 0;
        while (size--) n += write(*buffer++);
        return n;
    }
    size_t write(const char* str) { return str ? write(reinterpret_cast<const uint8_t*>(str), std::strlen(str)) : 0; }
    size_t write(const char* buffer, size_t size) { return write(reinterpret_cast<const uint8_t*>(buffer), size); }
    virtual void flush() {}

    size_t print(const String& s) { return write(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
    size_t print(const Printable& x) { return x.printTo(*this); }
    size_t print(const char* s) { return write(s); }
    size_t print(char c) { return write(static_cast<uint8_t>(c)); }
    size_t print(unsigned char v, int base = DEC) { return printNumber(v, base); }
    size_t print(int v, int base = DEC) { return printSigned(v, base); }
    size_t print(unsigned int v, int base = DEC) { return printNumber(v, base); }
    size_t print(long v, int base = DEC) { return printSigned(v, base); }
    size_t print(unsigned long v, int base = DEC) { return printNumber(v, base); }
    size_t print(long long v, int base = DEC) { return printSigned(v, base); }
    size_t print(unsigned long long v, int base = DEC) { return printNumber(v, base); }
    size_t print(double v, int digits = 2) { return print(String(v, static_cast<unsigned char>(digits))); }

    size_t println() { return write("\r\n"); }
    size_t println(const String& s) { return print(s) + println(); }
    size_t println(const Printable& x) { return print(x) + println(); }
    size_t println(const char* s) { return print(s) + println(); }
    size_t println(char c) { return print(c) + println(); }
    size_t println(unsigned char v, int base = DEC) { return print(v, base) + println(); }
    size_t println(int v, int base = DEC) { return print(v, base) + println(); }
    size_t println(unsigned int v, int base = DEC) { return print(v, base) + println(); }
    size_t println(long v, int base = DEC) { return print(v, base) + println(); }
    size_t println(unsigned long v, int base = DEC) { return print(v, base) + println(); }
    size_t println(long long v, int base = DEC) { return print(v, base) + println(); }
    size_t println(unsigned long long v, int base = DEC) { return print(v, base) + println(); }
    size_t println(double v, int digits = 2) { return print(v, digits) + println(); }

    size_t printf(const char* format, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap;
        va_start(ap, format);
        va_list ap2;
        va_copy(ap2, ap);
        char stack[256];
        int n = std::vsnprintf(stack, sizeof stack, format, ap);
        va_end(ap);
        size_t written = 0;
        if (n >= 0 && static_cast<size_t>(n) < sizeof stack) {
            written = write(reinterpret_cast<const uint8_t*>(stack), static_cast<size_t>(n));
        } else if (n > 0) {
            std::vector<char> big(static_cast<size_t>(n) + 1);
            std::vsnprintf(big.data(), big.size(), format, ap2);
            written = write(reinterpret_cast<const uint8_t*>(big.data()), static_cast<size_t>(n));
        }
        va_end(ap2);
        return written;
    }

private:
    size_t printNumber(unsigned long long v, int base) {
        if (base < 2 || base > 36) base = DEC;
        return print(String(v, static_cast<unsigned char>(base)));
    }
    size_t printSigned(long long v, int base) {
        if (base == DEC) return print(String(v));
        if (v < 0) return print('-') + printNumber(static_cast<unsigned long long>(-v), base);
        return printNumber(static_cast<unsigned long long>(v), base);
    }
};
