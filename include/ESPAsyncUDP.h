// ESPAsyncUDP emulation: AsyncUDP on top of Asio. IPv4 only.
//
// Callbacks run inside AsyncEmu::poll()/run() on the caller's thread.
//
// Limitations vs. the device library:
//  * AsyncUDPPacket::isBroadcast() is always false (the destination address
//    of a datagram is not available through Asio's portable API).
//  * AsyncUDPPacket::isMulticast() is true for every packet received on a
//    socket opened with listenMulticast().
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "AsyncEmu.h"
#include "IPAddress.h"

class AsyncUDP;

// Outgoing message builder (stands in for the Print-based original).
class AsyncUDPMessage {
public:
    AsyncUDPMessage(size_t size = 1460) : _cap(size) { _buf.reserve(size); }
    size_t write(uint8_t b) { return write(&b, 1); }
    size_t write(const uint8_t* data, size_t len) {
        size_t n = len < space() ? len : space();
        _buf.insert(_buf.end(), data, data + n);
        return n;
    }
    size_t print(const char* s);
    size_t space() const { return _cap - _buf.size(); }
    size_t length() const { return _buf.size(); }
    uint8_t* data() { return _buf.data(); }
    void flush() { _buf.clear(); }

private:
    size_t _cap;
    std::vector<uint8_t> _buf;
};

class AsyncUDPPacket {
public:
    AsyncUDPPacket(AsyncUDP* udp, uint8_t* data, size_t len, IPAddress localIp, uint16_t localPort,
                   IPAddress remoteIp, uint16_t remotePort, bool multicast)
        : _udp(udp), _data(data), _len(len), _lip(localIp), _lport(localPort),
          _rip(remoteIp), _rport(remotePort), _mcast(multicast) {}

    uint8_t* data() { return _data; }
    size_t length() { return _len; }
    bool isBroadcast() { return false; }
    bool isMulticast() { return _mcast; }
    IPAddress localIP() { return _lip; }
    uint16_t localPort() { return _lport; }
    IPAddress remoteIP() { return _rip; }
    uint16_t remotePort() { return _rport; }

    // Replies go back to the sender.
    size_t write(const uint8_t* data, size_t len);
    size_t write(uint8_t b) { return write(&b, 1); }
    size_t print(const char* s);
    size_t send(AsyncUDPMessage& message);

private:
    AsyncUDP* _udp;
    uint8_t* _data;
    size_t _len;
    IPAddress _lip;
    uint16_t _lport;
    IPAddress _rip;
    uint16_t _rport;
    bool _mcast;
};

typedef std::function<void(AsyncUDPPacket& packet)> AuPacketHandlerFunction;
typedef std::function<void(void* arg, AsyncUDPPacket& packet)> AuPacketHandlerFunctionWithArg;

class AsyncUDP {
public:
    AsyncUDP();
    ~AsyncUDP();
    AsyncUDP(const AsyncUDP&) = delete;
    AsyncUDP& operator=(const AsyncUDP&) = delete;

    void onPacket(AuPacketHandlerFunctionWithArg cb, void* arg);
    void onPacket(AuPacketHandlerFunction cb);

    bool connect(IPAddress addr, uint16_t port);  // sets the default peer for write()
    bool listen(IPAddress addr, uint16_t port);
    bool listen(uint16_t port);
    bool listenMulticast(IPAddress addr, uint16_t port, uint8_t ttl = 1);

    size_t writeTo(const uint8_t* data, size_t len, IPAddress addr, uint16_t port);
    size_t write(const uint8_t* data, size_t len);  // needs connect()
    size_t write(uint8_t data);
    size_t broadcastTo(uint8_t* data, size_t len, uint16_t port);
    size_t broadcastTo(const char* data, uint16_t port);
    size_t broadcast(uint8_t* data, size_t len);  // to the socket's local port
    size_t broadcast(const char* data);

    size_t send(AsyncUDPMessage& message);
    size_t sendTo(AsyncUDPMessage& message, IPAddress addr, uint16_t port);
    size_t broadcastTo(AsyncUDPMessage& message, uint16_t port);
    size_t broadcast(AsyncUDPMessage& message);

    void close();
    operator bool() const;
    uint16_t localPort() const;  // extension: bound port (listen(0) picks a free one)

    struct Impl;

private:
    std::shared_ptr<Impl> _i;
};
