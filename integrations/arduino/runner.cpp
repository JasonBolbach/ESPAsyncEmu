// Desktop entry point for Arduino-style sketches: setup() once, then loop()
// forever, pumping the emulated network between iterations.
//   sketch [--run-for <ms>] [--port-offset <n>]
//     --run-for      exit cleanly after <ms> (handy for smoke tests)
//     --port-offset  added to listening ports below 1024, so server(80) can run
//                    without root (see AsyncEmu::setPortOffset)
//     --profile      run under device limits: desktop (default), esp8266,
//                    esp8266-hb or esp32 (see AsyncEmu::setDeviceProfile)
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "Arduino.h"

int main(int argc, char** argv) {
    uint32_t runFor = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--run-for")) runFor = static_cast<uint32_t>(std::atoi(argv[i + 1]));
        if (!std::strcmp(argv[i], "--port-offset")) AsyncEmu::setPortOffset(static_cast<unsigned>(std::atoi(argv[i + 1])));
        if (!std::strcmp(argv[i], "--profile") && !AsyncEmu::setDeviceProfile(argv[i + 1])) {
            std::fprintf(stderr, "unknown profile '%s' (desktop, esp8266, esp8266-hb, esp32)\n", argv[i + 1]);
            return 2;
        }
    }

    setup();
    while (runFor == 0 || millis() < runFor) {
        loop();
        AsyncEmu::poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // don't spin a core
    }
    return 0;
}
