// Pump-mode integration without any GUI dependency: a fixed-rate "frame loop"
// calls AsyncEmu::poll() exactly once per frame, the way a game or GUI would.
// Network callbacks therefore run between frames on the render thread, so they
// may touch UI state without locks.
//
// Runs a loopback TCP echo for a few seconds and reports per-frame stats.
// Usage: frame_loop [frames]   (default 120 frames at ~60 fps)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "ESPAsyncTCP.h"

int main(int argc, char** argv) {
    int totalFrames = argc > 1 ? std::atoi(argv[1]) : 120;

    // "UI state" mutated directly from callbacks: safe, same thread.
    int messagesSeen = 0;
    int maxCallbacksInFrame = 0;

    AsyncServer server(0);
    server.onClient([](void*, AsyncClient* c) {
        c->onData([](void*, AsyncClient* cl, void* d, size_t n) { cl->write(static_cast<char*>(d), n); });
        c->onDisconnect([](void*, AsyncClient* cl) { delete cl; });
    });
    server.begin();

    AsyncClient client;
    client.onData([&](void*, AsyncClient*, void*, size_t) { ++messagesSeen; });
    client.connect(IPAddress(127, 0, 0, 1), server.port());

    using clock = std::chrono::steady_clock;
    const auto frameTime = std::chrono::microseconds(16667);
    auto next = clock::now();

    for (int frame = 0; frame < totalFrames; ++frame) {
        // --- network: run all ready callbacks (never blocks) ---
        std::size_t ran = AsyncEmu::poll();
        if (static_cast<int>(ran) > maxCallbacksInFrame) maxCallbacksInFrame = static_cast<int>(ran);

        // --- app logic: send something every 10 frames ---
        if (frame % 10 == 0 && client.connected()) client.write(("frame " + std::to_string(frame)).c_str());

        // --- "render" ---
        if (frame % 20 == 0)
            std::printf("frame %3d  connected=%d  echoes=%d\n", frame, client.connected() ? 1 : 0, messagesSeen);

        next += frameTime;
        std::this_thread::sleep_until(next);
    }
    std::printf("done: %d echoes, busiest frame ran %d handlers\n", messagesSeen, maxCallbacksInFrame);
    return messagesSeen > 0 ? 0 : 1;
}
