// Tiny Arduino-core shim so sketches written for ESPAsyncTCP/ESPAsyncUDP can
// run on the desktop. Provides millis/micros/delay/yield, a printf-capable
// Serial that writes to stdout, GPIO and ESP stubs. Include it only where you
// want an Arduino-like environment (ArduinoJson's Print/Stream support needs it).
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
#include "Stream.h"
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
    if (AsyncEmu::inCallback() && AsyncEmu::deviceLimits().warnDelayInCallback) {
        static bool warned = false;  // once is enough
        if (!warned) {
            warned = true;
            AsyncEmu::warn("delay() called inside a network callback. On the device this blocks the TCP task and trips the watchdog; "
                           "set a flag and do the work from loop() instead.");
        }
    }
    uint32_t end = millis() + ms;
    do {
        AsyncEmu::poll();
        if (static_cast<int32_t>(end - millis()) <= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (true);
}

// Serial: writes to stdout (a '\r' is dropped so Arduino's "\r\n" line endings look normal and
// logs stay clean when piped), flushing at every newline. Reading yields nothing.
struct SerialClass : public Stream {
    void begin(unsigned long) {}
    size_t write(uint8_t c) override {
        if (c == '\r') return 1;
        std::putchar(c);
        if (c == '\n') std::fflush(stdout);
        return 1;
    }
    using Print::write;
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override { std::fflush(stdout); }
    explicit operator bool() const { return true; }
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
