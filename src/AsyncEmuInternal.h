#pragma once
#include <asio.hpp>
#include <chrono>
#include <cstdint>

#include "IPAddress.h"

namespace AsyncEmu {
// The single shared Asio context all emulated sockets/timers live on.
asio::io_context& context();

// Applies the port offset (see AsyncEmu::setPortOffset) to a privileged listening port.
uint16_t mapPort(uint16_t port);

inline asio::ip::address_v4 toAsio(const IPAddress& ip) {
    asio::ip::address_v4::bytes_type b{{ip[0], ip[1], ip[2], ip[3]}};
    return asio::ip::address_v4(b);
}

inline IPAddress fromAsio(const asio::ip::address& a) {
    if (!a.is_v4()) return IPAddress();
    auto b = a.to_v4().to_bytes();
    return IPAddress(b[0], b[1], b[2], b[3]);
}

inline uint32_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
}  // namespace AsyncEmu
