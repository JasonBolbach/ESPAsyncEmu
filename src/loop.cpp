#include "AsyncEmu.h"
#include "AsyncEmuInternal.h"

#include <chrono>
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

void post(std::function<void()> fn) { asio::post(context(), std::move(fn)); }

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

}  // namespace AsyncEmu
