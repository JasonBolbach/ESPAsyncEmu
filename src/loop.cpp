#include "AsyncEmu.h"
#include "AsyncEmuInternal.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace AsyncEmu {

static std::unique_ptr<asio::io_context>& ctxPtr() {
    // concurrency hint 1: we are single threaded by design.
    static std::unique_ptr<asio::io_context> p(new asio::io_context(1));
    return p;
}

asio::io_context& context() { return *ctxPtr(); }

std::size_t poll() {
    auto& c = context();
    if (c.stopped()) c.restart();
    return c.poll();
}

std::size_t pollFor(unsigned timeoutMs) {
    auto& c = context();
    if (c.stopped()) c.restart();
    return c.run_for(std::chrono::milliseconds(timeoutMs));
}

void run() {
    auto& c = context();
    if (c.stopped()) c.restart();
    c.run();
}

void stop() { context().stop(); }

void post(std::function<void()> fn) {
    asio::post(context(), [fn = std::move(fn)] {
        CallbackScope scope;
        fn();
    });
}

void reset() { ctxPtr().reset(new asio::io_context(1)); }

static unsigned& offsetRef() {
    static unsigned v = [] {
        const char* e = std::getenv("ESPASYNCEMU_PORT_OFFSET");
        return e ? static_cast<unsigned>(std::strtoul(e, nullptr, 10)) : 0u;
    }();
    return v;
}

void setPortOffset(unsigned offset) { offsetRef() = offset; }
unsigned portOffset() { return offsetRef(); }

uint16_t mapPort(uint16_t port) {
    unsigned off = offsetRef();
    if (port == 0 || port >= 1024 || off == 0) return port;
    unsigned mapped = port + off;
    return mapped > 65535 ? port : static_cast<uint16_t>(mapped);
}


// ---- device limits ----------------------------------------------------------
DeviceLimits deviceProfile(const char* name, bool* known) {
    DeviceLimits l;  // desktop defaults
    std::string n = name ? name : "";
    bool ok = true;
    if (n == "desktop" || n.empty()) {
        // permissive defaults
    } else if (n == "esp8266") {
        l.sendBufferSize = 1072;  // 2 * MSS(536): lwIP v2 "lower memory" variant
        l.maxConnections = 5;     // MEMP_NUM_TCP_PCB
        l.strictHeaders = true;
        l.maxFormBodySize = 4096;
        l.watchdogMs = 3000;      // soft watchdog fires after ~3.2 s
        l.warnDelayInCallback = true;
    } else if (n == "esp8266-hb") {
        l.sendBufferSize = 2920;  // 2 * MSS(1460): lwIP "higher bandwidth" variant
        l.maxConnections = 5;
        l.strictHeaders = true;
        l.maxFormBodySize = 4096;
        l.watchdogMs = 3000;
        l.warnDelayInCallback = true;
    } else if (n == "esp32") {
        l.sendBufferSize = 5744;  // TCP_SND_BUF
        l.maxConnections = 16;    // CONFIG_LWIP_MAX_ACTIVE_TCP
        l.strictHeaders = true;
        l.maxFormBodySize = 32 * 1024;
        l.watchdogMs = 5000;      // async_tcp task watchdog
        l.warnDelayInCallback = true;
    } else {
        ok = false;
    }
    if (known) *known = ok;
    return l;
}

static DeviceLimits& limitsRef() {
    static DeviceLimits l = [] {
        const char* e = std::getenv("ESPASYNCEMU_PROFILE");
        bool known = true;
        DeviceLimits d = deviceProfile(e, &known);
        if (!known) std::fprintf(stderr, "[ESPAsyncEmu] unknown ESPASYNCEMU_PROFILE '%s', using desktop\n", e);
        return known ? d : DeviceLimits();
    }();
    return l;
}

const DeviceLimits& deviceLimits() { return limitsRef(); }
void setDeviceLimits(const DeviceLimits& limits) { limitsRef() = limits; }

bool setDeviceProfile(const char* name) {
    bool known = false;
    DeviceLimits l = deviceProfile(name, &known);
    if (known) limitsRef() = l;
    return known;
}

// ---- diagnostics ----------------------------------------------------------------
static std::function<void(const std::string&)>& warnHandler() {
    static std::function<void(const std::string&)> h;
    return h;
}

void setWarningHandler(std::function<void(const std::string&)> handler) { warnHandler() = std::move(handler); }

void warn(const std::string& message) {
    if (warnHandler()) warnHandler()(message);
    else std::fprintf(stderr, "[ESPAsyncEmu] %s\n", message.c_str());
}

// ---- callback tracking / watchdog -------------------------------------------------
static int g_callbackDepth = 0;
static std::chrono::steady_clock::time_point g_callbackStart;

bool inCallback() { return g_callbackDepth > 0; }

CallbackScope::CallbackScope() {
    if (g_callbackDepth++ == 0) g_callbackStart = std::chrono::steady_clock::now();
}

CallbackScope::~CallbackScope() {
    if (--g_callbackDepth != 0) return;
    unsigned limit = deviceLimits().watchdogMs;
    if (!limit) return;
    auto ms = static_cast<unsigned>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_callbackStart).count());
    if (ms > limit) {
        warn("watchdog: a network callback ran for " + std::to_string(ms) + " ms (limit " + std::to_string(limit) +
             " ms). On the device this resets the chip: never block (delay(), long loops, slow I/O) inside callbacks.");
        if (deviceLimits().abortOnWatchdog) std::abort();
    }
}

unsigned& activeConnections() {
    static unsigned n = 0;
    return n;
}

}  // namespace AsyncEmu
