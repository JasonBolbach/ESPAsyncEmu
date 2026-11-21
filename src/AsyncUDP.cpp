#include "ESPAsyncUDP.h"
#include "AsyncEmuInternal.h"

#include <cstring>
#include <vector>

#if !defined(_WIN32)
#include <sys/socket.h>
#endif

using asio::ip::udp;
using asio::error_code;
using namespace AsyncEmu;

// As in AsyncTCP.cpp: the Impl is shared with pending Asio handlers so the
// application may delete the AsyncUDP from inside its own packet callback.
struct AsyncUDP::Impl : std::enable_shared_from_this<AsyncUDP::Impl> {
    AsyncUDP* owner = nullptr;
    udp::socket sock;
    udp::endpoint sender;
    std::vector<uint8_t> buf;
    uint32_t gen = 0;
    bool receiving = false;
    bool connected = false;
    bool multicast = false;
    AuPacketHandlerFunctionWithArg cb;
    void* cbArg = nullptr;

    Impl() : sock(context()), buf(65536) {}

    // Open (unbound) if needed.
    bool ensureOpen() {
        if (sock.is_open()) return true;
        error_code ec;
        sock.open(udp::v4(), ec);
        return !ec;
    }

    // Open and bind to an ephemeral port if nothing has been bound yet.
    bool ensureBound() {
        if (!ensureOpen()) return false;
        error_code ec;
        udp::endpoint le = sock.local_endpoint(ec);
        if (!ec && le.port() != 0) return true;
        sock.bind(udp::endpoint(udp::v4(), 0), ec);
        return !ec;
    }

    void reset() {
        ++gen;
        receiving = false;
        connected = false;
        multicast = false;
        error_code ec;
        if (sock.is_open()) sock.close(ec);
    }

    bool bindTo(const udp::endpoint& ep, bool reusePort) {
        reset();
        error_code ec;
        sock.open(udp::v4(), ec);
        if (ec) return false;
        sock.set_option(udp::socket::reuse_address(true), ec);
#if defined(SO_REUSEPORT) && !defined(_WIN32)
        if (reusePort) {
            int one = 1;
            ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
        }
#else
        (void)reusePort;
#endif
        sock.bind(ep, ec);
        if (ec) { error_code ignored; sock.close(ignored); return false; }
        startRecv();
        return true;
    }

    void startRecv() {
        if (receiving) return;
        receiving = true;
        auto self = shared_from_this();
        uint32_t g = gen;
        sock.async_receive_from(asio::buffer(buf), sender, [self, g](const error_code& ec, size_t n) {
            if (g != self->gen || ec == asio::error::operation_aborted) return;
            self->receiving = false;
            if (ec) {
                // ICMP "port unreachable" on connected sockets is not fatal.
                if (ec == asio::error::connection_refused || ec == asio::error::connection_reset) self->startRecv();
                return;
            }
            error_code lec;
            udp::endpoint local = self->sock.local_endpoint(lec);
            AsyncUDPPacket pkt(self->owner, self->buf.data(), n, fromAsio(local.address()), local.port(),
                               fromAsio(self->sender.address()), self->sender.port(), self->multicast);
            auto cb = self->cb;
            void* arg = self->cbArg;
            if (cb && self->owner) cb(arg, pkt);
            if (!self->owner || self->gen != g) return;
            self->startRecv();
        });
    }

    size_t sendTo(const uint8_t* data, size_t len, const udp::endpoint& to) {
        if (!data || !len || !ensureBound()) return 0;
        startRecv();
        error_code ec;
        size_t n = sock.send_to(asio::buffer(data, len), to, 0, ec);
        return ec ? 0 : n;
    }
};

// ---------------------------------------------------------------------------
size_t AsyncUDPMessage::print(const char* s) { return s ? write(reinterpret_cast<const uint8_t*>(s), std::strlen(s)) : 0; }

size_t AsyncUDPPacket::write(const uint8_t* data, size_t len) { return _udp ? _udp->writeTo(data, len, _rip, _rport) : 0; }
size_t AsyncUDPPacket::print(const char* s) { return s ? write(reinterpret_cast<const uint8_t*>(s), std::strlen(s)) : 0; }
size_t AsyncUDPPacket::send(AsyncUDPMessage& m) { return write(m.data(), m.length()); }

// ---------------------------------------------------------------------------
AsyncUDP::AsyncUDP() : _i(std::make_shared<Impl>()) { _i->owner = this; }

AsyncUDP::~AsyncUDP() {
    _i->owner = nullptr;
    _i->reset();
}

void AsyncUDP::onPacket(AuPacketHandlerFunctionWithArg cb, void* arg) { _i->cb = std::move(cb); _i->cbArg = arg; }
void AsyncUDP::onPacket(AuPacketHandlerFunction cb) {
    _i->cb = [cb](void*, AsyncUDPPacket& p) { cb(p); };
    _i->cbArg = nullptr;
}

bool AsyncUDP::connect(IPAddress addr, uint16_t port) {
    if (!_i->ensureOpen()) return false;
    error_code ec;
    _i->sock.connect(udp::endpoint(toAsio(addr), port), ec);
    if (ec) return false;
    _i->connected = true;
    _i->startRecv();
    return true;
}

bool AsyncUDP::listen(IPAddress addr, uint16_t port) {
    return _i->bindTo(udp::endpoint(toAsio(addr), port), false);
}
bool AsyncUDP::listen(uint16_t port) { return listen(IPAddress(), port); }

bool AsyncUDP::listenMulticast(IPAddress addr, uint16_t port, uint8_t ttl) {
    if (!_i->bindTo(udp::endpoint(udp::v4(), port), true)) return false;
    error_code ec;
    _i->sock.set_option(asio::ip::multicast::join_group(toAsio(addr)), ec);
    if (ec) { _i->reset(); return false; }
    _i->sock.set_option(asio::ip::multicast::hops(ttl), ec);
    _i->multicast = true;
    return true;
}

size_t AsyncUDP::writeTo(const uint8_t* data, size_t len, IPAddress addr, uint16_t port) {
    return _i->sendTo(data, len, udp::endpoint(toAsio(addr), port));
}

size_t AsyncUDP::write(const uint8_t* data, size_t len) {
    if (!_i->connected || !data || !len) return 0;
    error_code ec;
    size_t n = _i->sock.send(asio::buffer(data, len), 0, ec);
    return ec ? 0 : n;
}
size_t AsyncUDP::write(uint8_t data) { return write(&data, 1); }

size_t AsyncUDP::broadcastTo(uint8_t* data, size_t len, uint16_t port) {
    if (!_i->ensureBound()) return 0;
    error_code ec;
    _i->sock.set_option(asio::socket_base::broadcast(true), ec);
    return writeTo(data, len, IPAddress(255, 255, 255, 255), port);
}
size_t AsyncUDP::broadcastTo(const char* data, uint16_t port) {
    return data ? broadcastTo(reinterpret_cast<uint8_t*>(const_cast<char*>(data)), std::strlen(data), port) : 0;
}
size_t AsyncUDP::broadcast(uint8_t* data, size_t len) {
    if (!_i->ensureBound()) return 0;
    return broadcastTo(data, len, localPort());
}
size_t AsyncUDP::broadcast(const char* data) {
    return data ? broadcast(reinterpret_cast<uint8_t*>(const_cast<char*>(data)), std::strlen(data)) : 0;
}

size_t AsyncUDP::send(AsyncUDPMessage& m) { return write(m.data(), m.length()); }
size_t AsyncUDP::sendTo(AsyncUDPMessage& m, IPAddress addr, uint16_t port) { return writeTo(m.data(), m.length(), addr, port); }
size_t AsyncUDP::broadcastTo(AsyncUDPMessage& m, uint16_t port) { return broadcastTo(m.data(), m.length(), port); }
size_t AsyncUDP::broadcast(AsyncUDPMessage& m) { return broadcast(m.data(), m.length()); }

void AsyncUDP::close() { _i->reset(); }
AsyncUDP::operator bool() const { return _i->sock.is_open(); }

uint16_t AsyncUDP::localPort() const {
    error_code ec;
    if (!_i->sock.is_open()) return 0;
    auto ep = _i->sock.local_endpoint(ec);
    return ec ? 0 : ep.port();
}
