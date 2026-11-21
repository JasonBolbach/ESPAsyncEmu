#include "AsyncWebSocket.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "WebUtil.h"

namespace {
uint32_t nowMillis() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Server -> client frames are never masked.
std::string buildFrame(uint8_t opcode, const uint8_t* data, size_t len) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));
    if (len < 126) {
        f.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        f.push_back(126);
        f.push_back(static_cast<char>(len >> 8));
        f.push_back(static_cast<char>(len & 0xFF));
    } else {
        f.push_back(127);
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (8 * i)) & 0xFF));
    }
    if (len) f.append(reinterpret_cast<const char*>(data), len);
    return f;
}

constexpr uint32_t kCloseWaitMs = 2000;  // after we send a close frame, wait this long for the peer's reply
}  // namespace

static std::string vformat(const char* format, va_list ap);

// ---------------------------------------------------------------------------
// AsyncWebSocketClient
// ---------------------------------------------------------------------------
AsyncWebSocketClient::AsyncWebSocketClient(AsyncWebServerRequest* request, AsyncWebSocket* server)
    : _client(request->client()), _server(server), _clientId(server->_getNextId()) {
    _lastActivity = nowMillis();
    _client->setRxTimeout(0);
    _client->onError([](void* r, AsyncClient*, int8_t err) { static_cast<AsyncWebSocketClient*>(r)->_onError(err); }, this);
    _client->onAck([](void* r, AsyncClient*, size_t len, uint32_t time) { static_cast<AsyncWebSocketClient*>(r)->_onAck(len, time); }, this);
    _client->onPoll([](void* r, AsyncClient*) { static_cast<AsyncWebSocketClient*>(r)->_onPoll(); }, this);
    _client->onTimeout([](void* r, AsyncClient*, uint32_t time) { static_cast<AsyncWebSocketClient*>(r)->_onTimeout(time); }, this);
    _client->onData([](void* r, AsyncClient*, void* buf, size_t len) { static_cast<AsyncWebSocketClient*>(r)->_onData(buf, len); }, this);
    _client->onDisconnect(
        [](void* r, AsyncClient* c) {
            static_cast<AsyncWebSocketClient*>(r)->_onDisconnect();  // deletes the client object
            delete c;
        },
        this);

    _server->_addClient(this);
    _server->_handleEvent(this, WS_EVT_CONNECT, request, nullptr, 0);
    // The HTTP request is finished: release it (it no longer owns the connection).
    request->_server->_handleDisconnect(request);
}

AsyncWebSocketClient::~AsyncWebSocketClient() {
    if (_alive) *_alive = false;
    _queue.clear();
}

void AsyncWebSocketClient::_queueFrame(uint8_t opcode, const uint8_t* data, size_t len, bool control) {
    if (!_client || _status == WS_DISCONNECTED) return;
    if (_status == WS_DISCONNECTING && opcode != WS_DISCONNECT) return;
    std::string f = buildFrame(opcode, data, len);
    if (control) {
        // Never split a data frame that is already partly on the wire.
        auto pos = _queue.begin();
        if (_offset > 0 && pos != _queue.end()) ++pos;
        _queue.insert(pos, std::move(f));
    } else {
        if (_queue.size() >= WS_MAX_QUEUED_MESSAGES) {  // like the device: drop when the queue is full
            AsyncEmu::warn("WebSocket client #" + std::to_string(_clientId) + ": message queue full (" + std::to_string(WS_MAX_QUEUED_MESSAGES) +
                           "), message dropped");
            return;
        }
        _queue.push_back(std::move(f));
    }
    _runQueue();
}

void AsyncWebSocketClient::_runQueue() {
    while (_client && !_queue.empty() && _client->canSend()) {
        std::string& m = _queue.front();
        size_t n = std::min(_client->space(), m.size() - _offset);
        size_t w = _client->write(m.data() + _offset, n);
        _offset += w;
        if (_offset >= m.size()) {
            _queue.pop_front();
            _offset = 0;
        } else {
            break;
        }
    }
    if (_client && _closeQueued && _queue.empty()) {
        _closeQueued = false;
        _client->close();  // graceful: flushes what was written
    }
}

void AsyncWebSocketClient::close(uint16_t code, const char* message) {
    if (_status != WS_CONNECTED) return;
    std::string payload;
    if (code) {
        payload.push_back(static_cast<char>(code >> 8));
        payload.push_back(static_cast<char>(code & 0xFF));
        if (message) payload += message;
    }
    _queueFrame(WS_DISCONNECT, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), true);
    _status = WS_DISCONNECTING;
    _closeDeadline = nowMillis() + kCloseWaitMs;  // then give up waiting for the peer's close frame
}

void AsyncWebSocketClient::ping(uint8_t* data, size_t len) {
    if (_status == WS_CONNECTED) _queueFrame(WS_PING, data, len, true);
}

void AsyncWebSocketClient::text(const char* message, size_t len) { if (_status == WS_CONNECTED) _queueFrame(WS_TEXT, reinterpret_cast<const uint8_t*>(message), len, false); }
void AsyncWebSocketClient::text(const char* message) { if (message) text(message, std::strlen(message)); }
void AsyncWebSocketClient::text(uint8_t* message, size_t len) { text(reinterpret_cast<const char*>(message), len); }
void AsyncWebSocketClient::text(const String& message) { text(message.c_str(), message.length()); }
void AsyncWebSocketClient::text(AsyncWebSocketMessageBuffer* buffer) {
    if (!buffer) return;
    text(reinterpret_cast<const char*>(buffer->get()), buffer->length());
    delete buffer;
}
void AsyncWebSocketClient::binary(const char* message, size_t len) { if (_status == WS_CONNECTED) _queueFrame(WS_BINARY, reinterpret_cast<const uint8_t*>(message), len, false); }
void AsyncWebSocketClient::binary(const char* message) { if (message) binary(message, std::strlen(message)); }
void AsyncWebSocketClient::binary(uint8_t* message, size_t len) { binary(reinterpret_cast<const char*>(message), len); }
void AsyncWebSocketClient::binary(const String& message) { binary(message.c_str(), message.length()); }
void AsyncWebSocketClient::binary(AsyncWebSocketMessageBuffer* buffer) {
    if (!buffer) return;
    binary(reinterpret_cast<const char*>(buffer->get()), buffer->length());
    delete buffer;
}

size_t AsyncWebSocketClient::printf(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    std::string s = vformat(format, ap);
    va_end(ap);
    text(s.c_str(), s.size());
    return s.size();
}

// ---- client events ------------------------------------------------------------
void AsyncWebSocketClient::_onAck(size_t, uint32_t) { _runQueue(); }

void AsyncWebSocketClient::_onError(int8_t err) { _server->_handleEvent(this, WS_EVT_ERROR, &err, nullptr, 0); }

void AsyncWebSocketClient::_onPoll() {
    _runQueue();
    uint32_t now = nowMillis();
    if (_status == WS_DISCONNECTING && _client && !_closeQueued && static_cast<int32_t>(now - _closeDeadline) > 0) {
        _client->close();  // the peer never answered our close frame
        return;
    }
    if (_keepAlive && _status == WS_CONNECTED && now - _lastActivity >= static_cast<uint32_t>(_keepAlive) * 1000u) {
        _lastActivity = now;
        ping();
    }
}

void AsyncWebSocketClient::_onTimeout(uint32_t) {
    if (_client) _client->close();
}

void AsyncWebSocketClient::_onDisconnect() {
    _client = nullptr;
    _status = WS_DISCONNECTED;
    _server->_handleEvent(this, WS_EVT_DISCONNECT, nullptr, nullptr, 0);
    _server->_handleDisconnect(this);  // deletes this
}

void AsyncWebSocketClient::_handleControl() {
    switch (_info.opcode) {
        case WS_DISCONNECT:
            if (_status == WS_CONNECTED) {
                // Echo the status code back and close once it has been flushed.
                std::string payload = _ctrl.size() >= 2 ? _ctrl.substr(0, 2) : std::string();
                _queueFrame(WS_DISCONNECT, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), true);
                _status = WS_DISCONNECTING;
                _closeQueued = true;
                _runQueue();
            } else if (_client) {
                _client->close();  // reply to the close we initiated
            }
            break;
        case WS_PING:
            _queueFrame(WS_PONG, reinterpret_cast<const uint8_t*>(_ctrl.data()), _ctrl.size(), true);
            break;
        case WS_PONG:
            _server->_handleEvent(this, WS_EVT_PONG, nullptr, reinterpret_cast<uint8_t*>(&_ctrl[0]), _ctrl.size());
            break;
        default:
            break;
    }
}

void AsyncWebSocketClient::_onData(void* pbuf, size_t plen) {
    _lastActivity = nowMillis();
    const uint8_t* data = static_cast<const uint8_t*>(pbuf);
    size_t len = plen;

    bool alive = true;
    _alive = &alive;  // set to false by the destructor if a handler destroys this client

    while (len > 0 && alive && _status != WS_DISCONNECTED) {
        if (!_inPayload) {
            // ---- frame header: 2 bytes, optional 2/8 byte length, optional 4 byte mask ----
            bool complete = false;
            while (len > 0 && !complete) {
                _hdr.push_back(static_cast<char>(*data++));
                --len;
                if (_hdr.size() < 2) continue;
                uint8_t b1 = static_cast<uint8_t>(_hdr[1]);
                bool masked = (b1 & 0x80) != 0;
                uint8_t l7 = b1 & 0x7F;
                size_t need = 2 + (l7 == 126 ? 2 : l7 == 127 ? 8 : 0) + (masked ? 4 : 0);
                if (_hdr.size() < need) continue;

                uint32_t prevNum = _info.num;
                uint8_t prevMsgOp = _info.message_opcode;
                std::memset(&_info, 0, sizeof _info);
                uint8_t b0 = static_cast<uint8_t>(_hdr[0]);
                _info.final = (b0 >> 7) & 1;
                _info.opcode = b0 & 0x0F;
                _info.masked = masked ? 1 : 0;
                uint64_t plen64 = l7;
                size_t pos = 2;
                if (l7 == 126) {
                    plen64 = (static_cast<uint64_t>(static_cast<uint8_t>(_hdr[2])) << 8) | static_cast<uint8_t>(_hdr[3]);
                    pos = 4;
                } else if (l7 == 127) {
                    plen64 = 0;
                    for (int i = 0; i < 8; ++i) plen64 = (plen64 << 8) | static_cast<uint8_t>(_hdr[2 + i]);
                    pos = 10;
                }
                if (masked) std::memcpy(_info.mask, &_hdr[pos], 4);
                _info.len = plen64;
                if (_info.opcode == WS_CONTINUATION) {
                    _info.message_opcode = prevMsgOp;
                    _info.num = prevNum + 1;
                } else if (_info.opcode < 8) {
                    _info.message_opcode = _info.opcode;
                    _info.num = 0;
                } else {
                    _info.message_opcode = prevMsgOp;  // control frames don't disturb the message state
                    _info.num = prevNum;
                }
                _hdr.clear();
                _received = 0;
                _ctrl.clear();
                _inPayload = true;
                complete = true;
            }
            if (!complete) break;  // need more header bytes

            if (_info.len == 0) {  // empty payload: the frame is already complete
                _inPayload = false;
                if (_info.opcode < 8) {
                    static uint8_t none[1] = {0};
                    _server->_handleEvent(this, WS_EVT_DATA, &_info, none, 0);
                } else {
                    _handleControl();
                }
            }
            continue;
        }

        // ---- payload ----
        uint64_t remaining = _info.len - _received;
        size_t n = static_cast<size_t>(std::min<uint64_t>(remaining, len));
        std::vector<uint8_t> chunk(data, data + n);
        if (_info.masked)
            for (size_t i = 0; i < n; ++i) chunk[i] ^= _info.mask[(_received + i) % 4];
        data += n;
        len -= n;

        if (_info.opcode < 8) {
            _info.index = _received;
            _received += n;
            if (_received >= _info.len) _inPayload = false;
            _server->_handleEvent(this, WS_EVT_DATA, &_info, chunk.data(), n);
        } else {
            _ctrl.append(reinterpret_cast<char*>(chunk.data()), n);
            _received += n;
            if (_received >= _info.len) {
                _inPayload = false;
                _handleControl();
            }
        }
    }
    if (alive) _alive = nullptr;
}

// ---------------------------------------------------------------------------
// AsyncWebSocket
// ---------------------------------------------------------------------------
AsyncWebSocket::~AsyncWebSocket() {
    _eventHandler = nullptr;  // no user callbacks while tearing down
    std::vector<AsyncWebSocketClient*> copy(_clients.begin(), _clients.end());
    for (auto* c : copy)
        if (c->client()) c->client()->close(true);  // -> _onDisconnect -> _handleDisconnect
}

AsyncWebSocketClient* AsyncWebSocket::client(uint32_t id) {
    for (auto* c : _clients)
        if (c->id() == id && c->status() == WS_CONNECTED) return c;
    return nullptr;
}

void AsyncWebSocket::close(uint32_t id, uint16_t code, const char* message) {
    if (auto* c = client(id)) c->close(code, message);
}

void AsyncWebSocket::closeAll(uint16_t code, const char* message) {
    std::vector<AsyncWebSocketClient*> copy(_clients.begin(), _clients.end());
    for (auto* c : copy)
        if (c->status() == WS_CONNECTED) c->close(code, message);
}

void AsyncWebSocket::cleanupClients(uint16_t maxClients) {
    if (count() > maxClients && !_clients.empty()) _clients.front()->close();
}

void AsyncWebSocket::ping(uint32_t id, uint8_t* data, size_t len) { if (auto* c = client(id)) c->ping(data, len); }
void AsyncWebSocket::pingAll(uint8_t* data, size_t len) {
    std::vector<AsyncWebSocketClient*> copy(_clients.begin(), _clients.end());
    for (auto* c : copy) if (c->status() == WS_CONNECTED) c->ping(data, len);
}

#define WS_FOR_ALL(expr)                                                                      \
    do {                                                                                      \
        std::vector<AsyncWebSocketClient*> copy_(_clients.begin(), _clients.end());           \
        for (auto* c : copy_) if (c->status() == WS_CONNECTED) { expr; }                      \
    } while (0)

void AsyncWebSocket::text(uint32_t id, const char* m, size_t len) { if (auto* c = client(id)) c->text(m, len); }
void AsyncWebSocket::text(uint32_t id, const char* m) { if (auto* c = client(id)) c->text(m); }
void AsyncWebSocket::text(uint32_t id, const String& m) { if (auto* c = client(id)) c->text(m); }
void AsyncWebSocket::text(uint32_t id, AsyncWebSocketMessageBuffer* b) {
    if (!b) return;
    if (auto* c = client(id)) c->text(reinterpret_cast<const char*>(b->get()), b->length());
    delete b;
}
void AsyncWebSocket::textAll(const char* m, size_t len) { WS_FOR_ALL(c->text(m, len)); }
void AsyncWebSocket::textAll(const char* m) { if (m) textAll(m, std::strlen(m)); }
void AsyncWebSocket::textAll(const String& m) { textAll(m.c_str(), m.length()); }
void AsyncWebSocket::textAll(AsyncWebSocketMessageBuffer* b) {
    if (!b) return;
    textAll(reinterpret_cast<const char*>(b->get()), b->length());
    delete b;
}
void AsyncWebSocket::binary(uint32_t id, const char* m, size_t len) { if (auto* c = client(id)) c->binary(m, len); }
void AsyncWebSocket::binary(uint32_t id, const char* m) { if (auto* c = client(id)) c->binary(m); }
void AsyncWebSocket::binary(uint32_t id, const String& m) { if (auto* c = client(id)) c->binary(m); }
void AsyncWebSocket::binary(uint32_t id, AsyncWebSocketMessageBuffer* b) {
    if (!b) return;
    if (auto* c = client(id)) c->binary(reinterpret_cast<const char*>(b->get()), b->length());
    delete b;
}
void AsyncWebSocket::binaryAll(const char* m, size_t len) { WS_FOR_ALL(c->binary(m, len)); }
void AsyncWebSocket::binaryAll(const char* m) { if (m) binaryAll(m, std::strlen(m)); }
void AsyncWebSocket::binaryAll(const String& m) { binaryAll(m.c_str(), m.length()); }
void AsyncWebSocket::binaryAll(AsyncWebSocketMessageBuffer* b) {
    if (!b) return;
    binaryAll(reinterpret_cast<const char*>(b->get()), b->length());
    delete b;
}

static std::string vformat(const char* format, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    char stack[256];
    int n = std::vsnprintf(stack, sizeof stack, format, ap);
    std::string out;
    if (n >= 0 && static_cast<size_t>(n) < sizeof stack) {
        out.assign(stack, static_cast<size_t>(n));
    } else if (n > 0) {
        out.resize(static_cast<size_t>(n) + 1);
        std::vsnprintf(&out[0], out.size(), format, ap2);
        out.resize(static_cast<size_t>(n));
    }
    va_end(ap2);
    return out;
}

size_t AsyncWebSocket::printf(uint32_t id, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    std::string s = vformat(format, ap);
    va_end(ap);
    text(id, s.c_str(), s.size());
    return s.size();
}

size_t AsyncWebSocket::printfAll(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    std::string s = vformat(format, ap);
    va_end(ap);
    textAll(s.c_str(), s.size());
    return s.size();
}

void AsyncWebSocket::_addClient(AsyncWebSocketClient* client) { _clients.push_back(client); }

void AsyncWebSocket::_handleDisconnect(AsyncWebSocketClient* client) {
    _clients.remove(client);
    delete client;
}

void AsyncWebSocket::_handleEvent(AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (_eventHandler) _eventHandler(this, client, type, arg, data, len);
}

bool AsyncWebSocket::canHandle(AsyncWebServerRequest* request) {
    if (!_enabled) return false;
    if (request->method() != HTTP_GET || !request->url().equals(_url) || !request->isExpectedRequestedConnType(RCT_WS)) return false;
    request->addInterestingHeader("Sec-WebSocket-Version");
    request->addInterestingHeader("Sec-WebSocket-Key");
    request->addInterestingHeader("Sec-WebSocket-Protocol");
    return true;
}

void AsyncWebSocket::handleRequest(AsyncWebServerRequest* request) {
    if (_username.length() && _password.length() && !request->authenticate(_username.c_str(), _password.c_str()))
        return request->requestAuthentication();

    AsyncWebHeader* version = request->getHeader("Sec-WebSocket-Version");
    if (!version || version->value().toInt() != 13) {
        AsyncWebServerResponse* response = request->beginResponse(400);
        response->addHeader("Sec-WebSocket-Version", "13");
        request->send(response);
        return;
    }
    AsyncWebHeader* key = request->getHeader("Sec-WebSocket-Key");
    if (!key) {
        request->send(400);
        return;
    }
    AsyncWebServerResponse* response = new AsyncWebSocketResponse(key->value(), this);
    if (request->hasHeader("Sec-WebSocket-Protocol")) response->addHeader("Sec-WebSocket-Protocol", request->header("Sec-WebSocket-Protocol"));
    request->send(response);
}

// ---------------------------------------------------------------------------
// AsyncWebSocketResponse
// ---------------------------------------------------------------------------
AsyncWebSocketResponse::AsyncWebSocketResponse(const String& key, AsyncWebSocket* server) : _server(server) {
    _code = 101;
    _sendContentLength = false;
    std::string accept = AsyncEmu::web::base64Encode(AsyncEmu::web::sha1Raw(std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    addHeader("Connection", "Upgrade");
    addHeader("Upgrade", "websocket");
    addHeader("Sec-WebSocket-Accept", String(accept));
}

void AsyncWebSocketResponse::_respond(AsyncWebServerRequest* request) {
    String head = _assembleHead(request->version());
    request->client()->write(head.c_str(), head.length());
    _state = RESPONSE_WAIT_ACK;
}

size_t AsyncWebSocketResponse::_ack(AsyncWebServerRequest* request, size_t len, uint32_t) {
    if (len) new AsyncWebSocketClient(request, _server);  // deletes the request (and this response)
    return 0;
}
