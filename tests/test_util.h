#pragma once
#include <chrono>
#include <cstdio>
#include <functional>

#include "AsyncEmu.h"

#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

// For use inside callbacks (lambdas can't `return 1`): records the failure.
inline bool& testFailed() { static bool f = false; return f; }
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); testFailed() = true; } } while (0)

// Pump the emulated network until pred() is true or timeoutMs elapses.
inline bool pumpUntil(const std::function<bool()>& pred, unsigned timeoutMs = 2000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) return false;
        AsyncEmu::pollFor(5);
    }
    return true;
}
