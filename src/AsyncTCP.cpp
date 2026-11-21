#include "ESPAsyncTCP.h"
#include "AsyncEmuInternal.h"

#include <algorithm>
#include <array>
#include <cstring>

using asio::ip::tcp;
using asio::error_code;
using namespace AsyncEmu;

namespace {

// lwip TCP states, as reported by AsyncClient::state().
enum : uint8_t {
    ST_CLOSED = 0, ST_LISTEN = 1, ST_SYN_SENT = 2, ST_SYN_RCVD = 3, ST_ESTABLISHED = 4,
    ST_FIN_WAIT_1 = 5, ST_FIN_WAIT_2 = 6, ST_CLOSE_WAIT = 7, ST_CLOSING = 8,
    ST_LAST_ACK = 9, ST_TIME_WAIT = 10
};

size_t sndBuf() { return deviceLimits().sendBufferSize; }  // per-connection TCP send buffer
constexpr unsigned kPollMs = 500;    // lwip coarse timer

int8_t mapError(const error_code& ec) {
    if (ec == asio::error::timed_out) return ERR_TIMEOUT;
    if (ec == asio::error::connection_refused || ec == asio::error::connection_reset) return ERR_RST;
    if (ec == asio::error::broken_pipe) return ERR_CLSD;
    if (ec == asio::error::host_unreachable || ec == asio::error::network_unreachable) return ERR_RTE;
    return ERR_ABRT;
}

}  // namespace

// ---------------------------------------------------------------------------
// AsyncClient::Impl
//
// Lives in a shared_ptr so that pending Asio handlers stay valid even if the
// application deletes the AsyncClient (typically inside one of its own
// callbacks). `owner` is nulled by ~AsyncClient; every callback site checks it
// afterwards. `gen` is bumped on every teardown so stale handlers bail out.
// ---------------------------------------------------------------------------
struct AsyncClient::Impl : std::enable_shared_from_this<AsyncClient::Impl> {
    AsyncClient* owner = nullptr;
    tcp::socket sock;
    tcp::resolver resolver;
    asio::steady_timer rxTimer, pollTimer;

    uint8_t st = ST_CLOSED;
    uint32_t gen = 0;
    bool closing = false;  // graceful close requested, waiting for writes to drain
    bool counted = false;  // accepted by an AsyncServer: counts towards DeviceLimits::maxConnections
    std::string queued;    // add()ed but not yet send()ed
    size_t inflight = 0;   // handed to Asio, not yet written
    std::array<char, 4096> rx;

    uint32_t rxTimeout = 0, ackTimeout = 5000, lastRx = 0;
    bool noDelay = false;
    IPAddress rip, lip;
    uint16_t rport = 0, lport = 0;

    AcConnectHandler connectCb, disconnectCb, pollCb;
    AcAckHandler ackCb;
    AcErrorHandler errorCb;
    AcDataHandler dataCb;
    AcTimeoutHandler timeoutCb;
    void *connectArg = nullptr, *disconnectArg = nullptr, *pollArg = nullptr, *ackArg = nullptr,
         *errorArg = nullptr, *dataArg = nullptr, *timeoutArg = nullptr;

    Impl() : sock(context()), resolver(context()), rxTimer(context()), pollTimer(context()) {}
    explicit Impl(tcp::socket&& s)
        : sock(std::move(s)), resolver(context()), rxTimer(context()), pollTimer(context()) {}

    // ---- connection setup -------------------------------------------------
    bool connectTo(const tcp::endpoint& ep) {
        if (st != ST_CLOSED) return false;
        sock = tcp::socket(context());
        st = ST_SYN_SENT;
        auto self = shared_from_this();
        uint32_t g = gen;
        sock.async_connect(ep, [self, g](const error_code& ec) {
            if (g != self->gen || ec == asio::error::operation_aborted) return;
            self->connected(ec);
        });
        return true;
    }

    bool connectHost(const std::string& host, uint16_t port) {
        if (st != ST_CLOSED) return false;
        error_code ip_ec;
        auto addr = asio::ip::make_address_v4(host, ip_ec);
        if (!ip_ec) return connectTo(tcp::endpoint(addr, port));

        sock = tcp::socket(context());
        st = ST_SYN_SENT;
        auto self = shared_from_this();
        uint32_t g = gen;
        resolver.async_resolve(tcp::v4(), host, std::to_string(port),
            [self, g](const error_code& ec, tcp::resolver::results_type res) {
                if (g != self->gen || ec == asio::error::operation_aborted) return;
                if (ec) { self->fail(ERR_ABRT); return; }
                asio::async_connect(self->sock, res,
                    [self, g](const error_code& ec2, const tcp::endpoint&) {
                        if (g != self->gen || ec2 == asio::error::operation_aborted) return;
                        self->connected(ec2);
                    });
            });
        return true;
    }

    void connected(const error_code& ec) {
        if (ec) { fail(mapError(ec)); return; }
        establish();
        auto self = shared_from_this();
        uint32_t g = gen;
        auto cb = connectCb;
        void* a = connectArg;
        if (cb && owner) { CallbackScope scope; cb(a, owner); }
        if (!owner || gen != g) return;
        start();
    }

    // Used by both outgoing connects and accepted sockets.
    void establish() {
        st = ST_ESTABLISHED;
        error_code ec;
        if (noDelay) sock.set_option(tcp::no_delay(true), ec);
        auto r = sock.remote_endpoint(ec);
        if (!ec) { rip = fromAsio(r.address()); rport = r.port(); }
        auto l = sock.local_endpoint(ec);
        if (!ec) { lip = fromAsio(l.address()); lport = l.port(); }
    }

    void start() {
        startRead();
        armRx();
        armPoll();
    }

    // ---- receive ----------------------------------------------------------
    void startRead() {
        auto self = shared_from_this();
        uint32_t g = gen;
        sock.async_read_some(asio::buffer(rx), [self, g](const error_code& ec, size_t n) {
            if (g != self->gen || ec == asio::error::operation_aborted) return;
            if (self->closing) return;  // draining for a graceful close
            if (ec) {
                if (ec == asio::error::eof) self->disconnected();
                else self->fail(mapError(ec));
                return;
            }
            self->armRx();
            auto cb = self->dataCb;
            void* a = self->dataArg;
            if (cb && self->owner) { CallbackScope scope; cb(a, self->owner, self->rx.data(), n); }
            if (!self->owner || self->gen != g || self->closing) return;
            self->startRead();
        });
    }

    void armRx() {
        rxTimer.cancel();
        lastRx = nowMs();
        if (rxTimeout == 0 || st != ST_ESTABLISHED) return;
        auto self = shared_from_this();
        uint32_t g = gen;
        rxTimer.expires_after(std::chrono::seconds(rxTimeout));
        rxTimer.async_wait([self, g](const error_code& ec) {
            if (ec || g != self->gen || self->st != ST_ESTABLISHED) return;
            auto cb = self->timeoutCb;
            void* a = self->timeoutArg;
            uint32_t elapsed = nowMs() - self->lastRx;
            if (cb && self->owner) { CallbackScope scope; cb(a, self->owner, elapsed); }
            if (!self->owner || self->gen != g) return;
            self->close(false);  // like the real library, an rx timeout closes the connection
        });
    }

    void armPoll() {
        pollTimer.cancel();
        if (!pollCb || st != ST_ESTABLISHED) return;
        auto self = shared_from_this();
        uint32_t g = gen;
        pollTimer.expires_after(std::chrono::milliseconds(kPollMs));
        pollTimer.async_wait([self, g](const error_code& ec) {
            if (ec || g != self->gen || self->st != ST_ESTABLISHED) return;
            auto cb = self->pollCb;
            void* a = self->pollArg;
            if (cb && self->owner) { CallbackScope scope; cb(a, self->owner); }
            if (!self->owner || self->gen != g) return;
            self->armPoll();
        });
    }

    // ---- send -------------------------------------------------------------
    size_t space() const {
        if (st != ST_ESTABLISHED || closing) return 0;
        size_t used = queued.size() + inflight;
        return used >= sndBuf() ? 0 : sndBuf() - used;
    }

    bool send() {
        if ((st != ST_ESTABLISHED && st != ST_FIN_WAIT_1) || queued.empty()) return false;
        auto buf = std::make_shared<std::string>(std::move(queued));
        queued.clear();
        inflight += buf->size();
        auto self = shared_from_this();
        uint32_t g = gen, t0 = nowMs();
        asio::async_write(sock, asio::buffer(*buf), [self, g, buf, t0](const error_code& ec, size_t n) {
            if (g != self->gen || ec == asio::error::operation_aborted) return;
            if (ec) { self->fail(mapError(ec)); return; }
            self->inflight -= n;
            auto cb = self->ackCb;
            void* a = self->ackArg;
            if (cb && self->owner) { CallbackScope scope; cb(a, self->owner, n, nowMs() - t0); }
            if (!self->owner || self->gen != g) return;
            if (self->closing && self->inflight == 0 && self->queued.empty()) self->disconnected();
        });
        return true;
    }

    // ---- closing ----------------------------------------------------------
    void close(bool now) {
        if (st == ST_CLOSED) return;
        if (now || st < ST_ESTABLISHED) { disconnected(); return; }
        if (closing) return;
        closing = true;
        st = ST_FIN_WAIT_1;
        rxTimer.cancel();
        pollTimer.cancel();
        send();  // flush anything that was add()ed
        if (inflight == 0) {
            auto self = shared_from_this();
            uint32_t g = gen;
            asio::post(context(), [self, g] { if (g == self->gen) self->disconnected(); });
        }
    }

    void teardown(bool rst = false) {
        ++gen;
        st = ST_CLOSED;
        closing = false;
        if (counted) { counted = false; --activeConnections(); }
        queued.clear();
        inflight = 0;
        error_code ec;
        rxTimer.cancel();
        pollTimer.cancel();
        resolver.cancel();
        if (sock.is_open()) {
            if (rst) sock.set_option(asio::socket_base::linger(true, 0), ec);
            else sock.shutdown(tcp::socket::shutdown_send, ec);
            sock.close(ec);
        }
    }

    void disconnected() {
        teardown();
        auto self = shared_from_this();
        auto cb = disconnectCb;
        void* a = disconnectArg;
        if (cb && owner) { CallbackScope scope; cb(a, owner); }
    }

    void fail(int8_t err, bool rst = false) {
        teardown(rst);
        auto self = shared_from_this();
        auto ecb = errorCb;
        void* ea = errorArg;
        if (ecb && owner) { CallbackScope scope; ecb(ea, owner, err); }
        auto dcb = disconnectCb;
        void* da = disconnectArg;
        if (dcb && owner) { CallbackScope scope; dcb(da, owner); }
    }
};

// ---------------------------------------------------------------------------
// AsyncClient
// ---------------------------------------------------------------------------
AsyncClient::AsyncClient(void*) : _i(std::make_shared<Impl>()) { _i->owner = this; }
AsyncClient::AsyncClient(std::shared_ptr<Impl> impl) : _i(std::move(impl)) { _i->owner = this; }

AsyncClient::~AsyncClient() {
    _i->owner = nullptr;  // pending handlers become no-ops
    _i->teardown();
}

bool AsyncClient::connect(IPAddress ip, uint16_t port) {
    return _i->connectTo(tcp::endpoint(toAsio(ip), port));
}
bool AsyncClient::connect(const char* host, uint16_t port) {
    return host && _i->connectHost(host, port);
}
void AsyncClient::close(bool now) { _i->close(now); }
void AsyncClient::stop() { _i->close(false); }

int8_t AsyncClient::abort() {
    if (_i->st != ST_CLOSED) _i->fail(ERR_ABRT, true);
    return ERR_ABRT;
}

bool AsyncClient::free() { return _i->st == ST_CLOSED || _i->st > ST_ESTABLISHED; }
bool AsyncClient::freeable() { return _i->st == ST_CLOSED || _i->st > ST_ESTABLISHED; }

bool AsyncClient::canSend() { return space() > 0; }
size_t AsyncClient::space() { return _i->space(); }

size_t AsyncClient::add(const char* data, size_t size, uint8_t) {
    if (!data || size == 0) return 0;
    size_t n = std::min(size, _i->space());
    _i->queued.append(data, n);
    return n;
}

bool AsyncClient::send() { return _i->send(); }
size_t AsyncClient::write(const char* data) { return data ? write(data, std::strlen(data)) : 0; }

size_t AsyncClient::write(const char* data, size_t size, uint8_t apiflags) {
    size_t n = add(data, size, apiflags);
    if (n && !(apiflags & ASYNC_WRITE_FLAG_MORE)) send();
    return n;
}

uint8_t AsyncClient::state() { return _i->st; }
bool AsyncClient::connecting() { return _i->st > 0 && _i->st < ST_ESTABLISHED; }
bool AsyncClient::connected() { return _i->st == ST_ESTABLISHED; }
bool AsyncClient::disconnecting() { return _i->st > ST_ESTABLISHED && _i->st < ST_TIME_WAIT; }
bool AsyncClient::disconnected() { return _i->st == ST_CLOSED || _i->st == ST_TIME_WAIT; }

uint32_t AsyncClient::getRemoteAddress() { return _i->rip; }
uint16_t AsyncClient::getRemotePort() { return _i->rport; }
uint32_t AsyncClient::getLocalAddress() { return _i->lip; }
uint16_t AsyncClient::getLocalPort() { return _i->lport; }
IPAddress AsyncClient::remoteIP() { return _i->rip; }
uint16_t AsyncClient::remotePort() { return _i->rport; }
IPAddress AsyncClient::localIP() { return _i->lip; }
uint16_t AsyncClient::localPort() { return _i->lport; }

void AsyncClient::setRxTimeout(uint32_t t) {
    _i->rxTimeout = t;
    if (_i->st == ST_ESTABLISHED) _i->armRx();
}
uint32_t AsyncClient::getRxTimeout() { return _i->rxTimeout; }
void AsyncClient::setAckTimeout(uint32_t t) { _i->ackTimeout = t; }
uint32_t AsyncClient::getAckTimeout() { return _i->ackTimeout; }

void AsyncClient::setNoDelay(bool nodelay) {
    _i->noDelay = nodelay;
    if (_i->sock.is_open()) { error_code ec; _i->sock.set_option(tcp::no_delay(nodelay), ec); }
}
bool AsyncClient::getNoDelay() { return _i->noDelay; }

void AsyncClient::onConnect(AcConnectHandler cb, void* arg) { _i->connectCb = std::move(cb); _i->connectArg = arg; }
void AsyncClient::onDisconnect(AcConnectHandler cb, void* arg) { _i->disconnectCb = std::move(cb); _i->disconnectArg = arg; }
void AsyncClient::onAck(AcAckHandler cb, void* arg) { _i->ackCb = std::move(cb); _i->ackArg = arg; }
void AsyncClient::onError(AcErrorHandler cb, void* arg) { _i->errorCb = std::move(cb); _i->errorArg = arg; }
void AsyncClient::onData(AcDataHandler cb, void* arg) { _i->dataCb = std::move(cb); _i->dataArg = arg; }
void AsyncClient::onTimeout(AcTimeoutHandler cb, void* arg) { _i->timeoutCb = std::move(cb); _i->timeoutArg = arg; }
void AsyncClient::onPoll(AcConnectHandler cb, void* arg) {
    _i->pollCb = std::move(cb);
    _i->pollArg = arg;
    if (_i->st == ST_ESTABLISHED) _i->armPoll();
}

const char* AsyncClient::errorToString(int8_t error) {
    switch (error) {
        case ERR_OK: return "OK";
        case ERR_MEM: return "Out of memory error";
        case ERR_BUF: return "Buffer error";
        case ERR_TIMEOUT: return "Timeout";
        case ERR_RTE: return "Routing problem";
        case ERR_INPROGRESS: return "Operation in progress";
        case ERR_VAL: return "Illegal value";
        case ERR_WOULDBLOCK: return "Operation would block";
        case ERR_ABRT: return "Connection aborted";
        case ERR_RST: return "Connection reset";
        case ERR_CLSD: return "Connection closed";
        case ERR_CONN: return "Not connected";
        case ERR_ARG: return "Illegal argument";
        case ERR_IF: return "Low-level netif error";
        default: return "UNKNOWN";
    }
}

const char* AsyncClient::stateToString() {
    switch (_i->st) {
        case ST_CLOSED: return "Closed";
        case ST_LISTEN: return "Listen";
        case ST_SYN_SENT: return "SYN Sent";
        case ST_SYN_RCVD: return "SYN Received";
        case ST_ESTABLISHED: return "Established";
        case ST_FIN_WAIT_1: return "FIN Wait 1";
        case ST_FIN_WAIT_2: return "FIN Wait 2";
        case ST_CLOSE_WAIT: return "Close Wait";
        case ST_CLOSING: return "Closing";
        case ST_LAST_ACK: return "Last ACK";
        case ST_TIME_WAIT: return "Time Wait";
        default: return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// AsyncServer
// ---------------------------------------------------------------------------
struct AsyncServer::Impl : std::enable_shared_from_this<AsyncServer::Impl> {
    AsyncServer* owner = nullptr;
    tcp::acceptor acceptor;
    IPAddress addr;
    uint16_t port = 0;
    uint32_t gen = 0;
    bool listening = false;
    bool noDelay = false;
    AcConnectHandler clientCb;
    void* clientArg = nullptr;

    Impl(IPAddress a, uint16_t p) : acceptor(context()), addr(a), port(p) {}

    void begin() {
        if (listening) return;
        error_code ec;
        tcp::endpoint ep(toAsio(addr), mapPort(port));
        acceptor.open(ep.protocol(), ec);
        if (ec) return;
        acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
        acceptor.bind(ep, ec);
        if (!ec) acceptor.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) { acceptor.close(ec); return; }
        port = acceptor.local_endpoint().port();
        listening = true;
        accept();
    }

    void end() {
        ++gen;
        listening = false;
        error_code ec;
        if (acceptor.is_open()) acceptor.close(ec);
    }

    void accept() {
        auto self = shared_from_this();
        uint32_t g = gen;
        acceptor.async_accept([self, g](const error_code& ec, tcp::socket sock) {
            if (g != self->gen || ec == asio::error::operation_aborted) return;
            if (!ec) self->handleClient(std::move(sock));
            if (self->owner && self->gen == g && self->listening) self->accept();
        });
    }

    void handleClient(tcp::socket&& sock) {
        unsigned limit = deviceLimits().maxConnections;
        if (limit && activeConnections() >= limit) {
            warn("connection from a client refused: device limit of " + std::to_string(limit) +
                 " simultaneous server connections reached (DeviceLimits::maxConnections)");
            return;  // the socket is closed on scope exit, like a full PCB table
        }
        auto ci = std::make_shared<AsyncClient::Impl>(std::move(sock));
        ci->noDelay = noDelay;
        ci->counted = true;
        ++activeConnections();
        ci->establish();
        auto* c = new AsyncClient(ci);
        auto cb = clientCb;
        void* a = clientArg;
        if (!cb) { delete c; return; }
        { CallbackScope scope; cb(a, c); }
        // The application may have deleted the client inside the callback.
        if (ci->owner) ci->start();
    }
};

AsyncServer::AsyncServer(IPAddress addr, uint16_t port) : _i(std::make_shared<Impl>(addr, port)) { _i->owner = this; }
AsyncServer::AsyncServer(uint16_t port) : AsyncServer(IPAddress(), port) {}

AsyncServer::~AsyncServer() {
    _i->owner = nullptr;
    _i->end();
}

void AsyncServer::onClient(AcConnectHandler cb, void* arg) { _i->clientCb = std::move(cb); _i->clientArg = arg; }
void AsyncServer::begin() { _i->begin(); }
void AsyncServer::end() { _i->end(); }
void AsyncServer::setNoDelay(bool nodelay) { _i->noDelay = nodelay; }
bool AsyncServer::getNoDelay() { return _i->noDelay; }
uint8_t AsyncServer::status() { return _i->listening ? ST_LISTEN : ST_CLOSED; }
uint16_t AsyncServer::port() { return _i->port; }
