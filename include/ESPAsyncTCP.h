// ESPAsyncTCP emulation: AsyncClient / AsyncServer on top of Asio.
//
// Same public API as me-no-dev/ESPAsyncTCP (minus lwip-specific bits such as
// tcp_pcb and pbuf packets). All callbacks run inside AsyncEmu::poll()/run()
// on the caller's thread.
//
// Differences worth knowing:
//  * IPv4 only. Host names are resolved asynchronously.
//  * AsyncServer::port() is an extension that returns the bound port, so
//    AsyncServer(0) can be used to pick a free port (handy in tests).
//  * Error codes follow lwip 1.4 numbering (see the enum below).
//  * Do not call AsyncEmu::reset() while clients/servers exist.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "AsyncEmu.h"
#include "IPAddress.h"

#define ASYNC_WRITE_FLAG_COPY 0x01  // accepted for compatibility; data is always copied
#define ASYNC_WRITE_FLAG_MORE 0x02  // more data follows: write() won't flush yet

typedef int8_t err_t;
enum {
    ERR_OK = 0,
    ERR_MEM = -1,
    ERR_BUF = -2,
    ERR_TIMEOUT = -3,
    ERR_RTE = -4,
    ERR_INPROGRESS = -5,
    ERR_VAL = -6,
    ERR_WOULDBLOCK = -7,
    ERR_ABRT = -8,
    ERR_RST = -9,
    ERR_CLSD = -10,
    ERR_CONN = -11,
    ERR_ARG = -12,
    ERR_IF = -13
};

class AsyncClient;

typedef std::function<void(void*, AsyncClient*)> AcConnectHandler;
typedef std::function<void(void*, AsyncClient*, size_t len, uint32_t time)> AcAckHandler;
typedef std::function<void(void*, AsyncClient*, int8_t error)> AcErrorHandler;
typedef std::function<void(void*, AsyncClient*, void* data, size_t len)> AcDataHandler;
typedef std::function<void(void*, AsyncClient*, uint32_t time)> AcTimeoutHandler;

class AsyncClient {
public:
    AsyncClient(void* pcb = nullptr);  // pcb is ignored (lwip compatibility)
    ~AsyncClient();
    AsyncClient(const AsyncClient&) = delete;
    AsyncClient& operator=(const AsyncClient&) = delete;

    bool connect(IPAddress ip, uint16_t port);
    bool connect(const char* host, uint16_t port);
    void close(bool now = false);  // graceful by default: flushes pending data first
    void stop();                   // same as close()
    int8_t abort();                // reset the connection, fires onError(ERR_ABRT)
    bool free();                   // true when no connection exists / is closing
    bool freeable();

    bool canSend();
    size_t space();  // free room in the send buffer
    size_t add(const char* data, size_t size, uint8_t apiflags = ASYNC_WRITE_FLAG_COPY);
    bool send();
    size_t write(const char* data);
    size_t write(const char* data, size_t size, uint8_t apiflags = ASYNC_WRITE_FLAG_COPY);

    uint8_t state();
    bool connecting();
    bool connected();
    bool disconnecting();
    bool disconnected();

    uint32_t getRemoteAddress();
    uint16_t getRemotePort();
    uint32_t getLocalAddress();
    uint16_t getLocalPort();
    IPAddress remoteIP();
    uint16_t remotePort();
    IPAddress localIP();
    uint16_t localPort();

    void setRxTimeout(uint32_t timeout);  // seconds, 0 = off
    uint32_t getRxTimeout();
    void setAckTimeout(uint32_t timeout);  // ms (stored only)
    uint32_t getAckTimeout();
    void setNoDelay(bool nodelay);
    bool getNoDelay();

    void onConnect(AcConnectHandler cb, void* arg = nullptr);
    void onDisconnect(AcConnectHandler cb, void* arg = nullptr);
    void onAck(AcAckHandler cb, void* arg = nullptr);
    void onError(AcErrorHandler cb, void* arg = nullptr);
    void onData(AcDataHandler cb, void* arg = nullptr);
    void onTimeout(AcTimeoutHandler cb, void* arg = nullptr);
    void onPoll(AcConnectHandler cb, void* arg = nullptr);  // ~every 500 ms while connected

    const char* errorToString(int8_t error);
    const char* stateToString();

    struct Impl;

private:
    friend class AsyncServer;
    explicit AsyncClient(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> _i;
};

class AsyncServer {
public:
    AsyncServer(IPAddress addr, uint16_t port);
    AsyncServer(uint16_t port);
    ~AsyncServer();
    AsyncServer(const AsyncServer&) = delete;
    AsyncServer& operator=(const AsyncServer&) = delete;

    // The callback receives a heap-allocated AsyncClient. As with the real
    // library the application owns it (usually `delete c` in onDisconnect).
    void onClient(AcConnectHandler cb, void* arg = nullptr);
    void begin();
    void end();
    void setNoDelay(bool nodelay);
    bool getNoDelay();
    uint8_t status();   // 0 = closed, 1 = listening
    uint16_t port();    // extension: actual bound port (after begin())

    struct Impl;

private:
    std::shared_ptr<Impl> _i;
};
