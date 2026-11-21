// ESPAsyncEmu - event loop control.
//
// The emulation layer is driven by the host application. Call
// AsyncEmu::poll() regularly (e.g. once per frame of a UI loop) and every
// network callback (onConnect, onData, onPacket, ...) runs inside that call,
// on the caller's thread. No locking is needed in user code.
#pragma once
#include <cstddef>
#include <functional>
#include <string>

namespace AsyncEmu {

// Run all ready handlers without blocking. Returns how many ran.
// This is the primary entry point ("pump" mode).
std::size_t poll();

// Like poll(), but waits up to timeoutMs for at least one handler.
std::size_t pollFor(unsigned timeoutMs);

// Block and run until stop() is called or no work remains (headless tools).
void run();

// Make run() return. Safe to call from within a callback.
void stop();

// Queue a function to run on the next poll()/run(). Handy for deferring work.
void post(std::function<void()> fn);

// Drop all pending work and restart the loop (mainly for tests).
void reset();

// Sketches commonly listen on privileged ports (AsyncWebServer server(80)), which
// an unprivileged desktop process usually can't bind. This offset is added to every
// *listening* port below 1024 (AsyncServer, AsyncWebServer, AsyncUDP::listen).
// Initial value: the ESPASYNCEMU_PORT_OFFSET environment variable, else 0.
// Example: setPortOffset(8000) makes server(80) listen on 8080.
void setPortOffset(unsigned offset);
unsigned portOffset();

// ---------------------------------------------------------------------------
// Device limits ("strict mode")
//
// A desktop is far more generous than an ESP. These limits let you run the same
// code under constraints that resemble the real chips, so problems show up on
// the desktop instead of on the board. Everything defaults to the permissive
// "desktop" profile. See wiki/Device-Profiles.md. The numbers in the esp8266 /
// esp32 profiles are approximations of common default configurations.
// ---------------------------------------------------------------------------
struct DeviceLimits {
    size_t sendBufferSize = 5744;         // per-connection TCP send buffer (AsyncClient::space())
    unsigned maxConnections = 0;          // simultaneous TCP connections, 0 = unlimited; extra ones are dropped
    bool strictHeaders = false;           // AsyncWebServer keeps only "interesting" request headers
    size_t maxFormBodySize = 1024 * 1024;  // largest urlencoded request body AsyncWebServer will buffer
    unsigned watchdogMs = 0;              // a network callback running longer than this is reported, 0 = off
    bool abortOnWatchdog = false;         // abort() instead of just warning (like the chip resetting)
    bool warnDelayInCallback = false;     // the Arduino shim's delay() warns when called inside a callback
};

const DeviceLimits& deviceLimits();
void setDeviceLimits(const DeviceLimits& limits);

// Selects a preset: "desktop", "esp8266" (default lwIP variant), "esp8266-hb"
// (lwIP higher bandwidth) or "esp32". Returns false for an unknown name.
// The initial profile comes from the ESPASYNCEMU_PROFILE environment variable.
bool setDeviceProfile(const char* name);
DeviceLimits deviceProfile(const char* name, bool* known = nullptr);

// True while a user callback (onData, onClient, onPacket, post()ed function, ...)
// is running. On a device these run in the network task, so blocking there is a bug.
bool inCallback();

// Diagnostics from the emulation (watchdog, dropped connections, overflowing queues, ...)
// go to stderr unless a handler is installed (handy in tests).
void setWarningHandler(std::function<void(const std::string&)> handler);
void warn(const std::string& message);

}  // namespace AsyncEmu
