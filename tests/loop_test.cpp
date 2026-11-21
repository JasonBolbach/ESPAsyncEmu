#include "AsyncEmu.h"
#include <cstdio>
#include <thread>

#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main() {
    // poll() with nothing queued returns 0 and doesn't block.
    CHECK(AsyncEmu::poll() == 0);

    // post() runs on the caller's thread, during poll().
    int hits = 0;
    auto self = std::this_thread::get_id();
    bool sameThread = false;
    AsyncEmu::post([&] { ++hits; sameThread = (std::this_thread::get_id() == self); });
    CHECK(hits == 0);            // nothing runs until we pump
    AsyncEmu::poll();
    CHECK(hits == 1);
    CHECK(sameThread);

    // Handlers can re-post; run() ends on stop().
    int n = 0;
    std::function<void()> tick = [&] { if (++n == 3) AsyncEmu::stop(); else AsyncEmu::post(tick); };
    AsyncEmu::post(tick);
    AsyncEmu::run();
    CHECK(n == 3);

    // Loop is reusable after stop().
    AsyncEmu::post([&] { ++hits; });
    AsyncEmu::poll();
    CHECK(hits == 2);

    std::puts("loop_test OK");
    return 0;
}
