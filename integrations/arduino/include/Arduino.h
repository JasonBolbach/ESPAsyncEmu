// Tiny Arduino-core shim so sketches written for ESPAsyncTCP/ESPAsyncUDP can
// run on the desktop. Provides millis/micros/delay/yield and a printf-capable
// Serial that writes to stdout.
//
// delay() keeps the emulated network alive (it pumps AsyncEmu::poll() while it
// waits), just like delay() lets the ESP's background tasks run.
#pragma once
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "AsyncEmu.h"
#include "IPAddress.h"
#include "WString.h"

inline uint32_t millis() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now() - start).count());
}

inline uint32_t micros() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return static_cast<uint32_t>(duration_cast<microseconds>(steady_clock::now() - start).count());
}

inline void yield() { AsyncEmu::poll(); }

inline void delay(uint32_t ms) {
    uint32_t end = millis() + ms;
    do {
        AsyncEmu::poll();
        if (static_cast<int32_t>(end - millis()) <= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (true);
}

struct SerialClass {
    void begin(unsigned long) {}
    size_t print(const char* s) { return static_cast<size_t>(std::fputs(s, stdout) >= 0 ? std::char_traits<char>::length(s) : 0); }
    size_t print(const std::string& s) { return print(s.c_str()); }
    size_t print(int v) { return static_cast<size_t>(std::printf("%d", v)); }
    size_t print(unsigned v) { return static_cast<size_t>(std::printf("%u", v)); }
    size_t println(const char* s = "") { size_t n = print(s); std::putchar('\n'); std::fflush(stdout); return n + 1; }
    size_t println(const std::string& s) { return println(s.c_str()); }
    size_t println(int v) { size_t n = print(v); std::putchar('\n'); std::fflush(stdout); return n + 1; }
    size_t println(unsigned v) { size_t n = print(v); std::putchar('\n'); std::fflush(stdout); return n + 1; }
    int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap;
        va_start(ap, fmt);
        int n = std::vprintf(fmt, ap);
        va_end(ap);
        std::fflush(stdout);
        return n;
    }
    void flush() { std::fflush(stdout); }
};
inline SerialClass Serial;

// ---- misc Arduino core stubs -------------------------------------------------------
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
enum { INPUT = 0, OUTPUT = 1, INPUT_PULLUP = 2, LOW = 0, HIGH = 1 };
inline void pinMode(uint8_t, uint8_t) {}
inline void digitalWrite(uint8_t pin, uint8_t val) { std::printf("[gpio] pin %u -> %s\n", pin, val ? "HIGH" : "LOW"); }
inline int digitalRead(uint8_t) { return 0; }
inline int analogRead(uint8_t) { return 512; }

struct EspClass {
    uint32_t getFreeHeap() { return 48000; }
    uint32_t getChipId() { return 0xE5B0; }
    uint32_t getCycleCount() { return micros() * 80; }
    void restart() { std::puts("[esp] restart requested, exiting"); std::exit(0); }
};
inline EspClass ESP;

// Provided by the sketch.
void setup();
void loop();
