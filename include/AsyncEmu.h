// ESPAsyncEmu - event loop control.
//
// The emulation layer is driven by the host application. Call
// AsyncEmu::poll() regularly (e.g. once per frame of a UI loop) and every
// network callback (onConnect, onData, onPacket, ...) runs inside that call,
// on the caller's thread. No locking is needed in user code.
#pragma once
#include <cstddef>
#include <functional>

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

}  // namespace AsyncEmu
