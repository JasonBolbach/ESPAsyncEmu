#include "AsyncEventSource.h"

#include <algorithm>
#include <cstdlib>

// "retry: / id: / event: / data: ..." framing; multi-line data becomes several data: lines.
static std::string generateEventMessage(const char* message, const char* event, uint32_t id, uint32_t reconnect) {
    std::string out;
    if (reconnect) out += "retry: " + std::to_string(reconnect) + "\r\n";
    if (id) out += "id: " + std::to_string(id) + "\r\n";
    if (event && *event) out += std::string("event: ") + event + "\r\n";
    if (message) {
        const char* p = message;
        while (true) {
            const char* e = p;
            while (*e && *e != '\r' && *e != '\n') ++e;
            out += "data: ";
            out.append(p, static_cast<size_t>(e - p));
            out += "\r\n";
            if (!*e) break;
            if (e[0] == '\r' && e[1] == '\n') ++e;
            p = e + 1;
            if (!*p && (e[0] == '\n' || e[0] == '\r')) break;  // trailing newline: no empty extra line
        }
    }
    out += "\r\n";
    return out;
}

// ---------------------------------------------------------------------------
// AsyncEventSourceClient
// ---------------------------------------------------------------------------
AsyncEventSourceClient::AsyncEventSourceClient(AsyncWebServerRequest* request, AsyncEventSource* server)
    : _client(request->client()), _server(server) {
    if (request->hasHeader("Last-Event-ID")) _lastId = static_cast<uint32_t>(std::atoi(request->header("Last-Event-ID").c_str()));

    _client->setRxTimeout(0);
    _client->onError(nullptr, nullptr);
    _client->onData(nullptr, nullptr);
    _client->onAck([](void* r, AsyncClient*, size_t len, uint32_t time) { static_cast<AsyncEventSourceClient*>(r)->_onAck(len, time); }, this);
    _client->onPoll([](void* r, AsyncClient*) { static_cast<AsyncEventSourceClient*>(r)->_onPoll(); }, this);
    _client->onTimeout([](void* r, AsyncClient*, uint32_t time) { static_cast<AsyncEventSourceClient*>(r)->_onTimeout(time); }, this);
    _client->onDisconnect(
        [](void* r, AsyncClient* c) {
            static_cast<AsyncEventSourceClient*>(r)->_onDisconnect();  // deletes the client object
            delete c;
        },
        this);

    _server->_addClient(this);
    // The HTTP request is finished: release it (it no longer owns the connection).
    request->_server->_handleDisconnect(request);
}

AsyncEventSourceClient::~AsyncEventSourceClient() { _queue.clear(); }

void AsyncEventSourceClient::close() {
    if (_client) _client->close();
}

void AsyncEventSourceClient::write(const char* message, size_t len) {
    if (!_client || !connected()) return;
    if (_queue.size() >= SSE_MAX_QUEUED_MESSAGES) return;  // like the device: drop when the queue is full
    _queue.emplace_back(message, len);
    _runQueue();
}

void AsyncEventSourceClient::send(const char* message, const char* event, uint32_t id, uint32_t reconnect) {
    std::string m = generateEventMessage(message, event, id, reconnect);
    write(m.data(), m.size());
}

void AsyncEventSourceClient::_runQueue() {
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
}

void AsyncEventSourceClient::_onAck(size_t, uint32_t) { _runQueue(); }
void AsyncEventSourceClient::_onPoll() { _runQueue(); }
void AsyncEventSourceClient::_onTimeout(uint32_t) {
    if (_client) _client->close();
}

void AsyncEventSourceClient::_onDisconnect() {
    _client = nullptr;
    _server->_handleDisconnect(this);  // deletes this
}

// ---------------------------------------------------------------------------
// AsyncEventSource
// ---------------------------------------------------------------------------
AsyncEventSource::~AsyncEventSource() { close(); }

void AsyncEventSource::close() {
    std::vector<AsyncEventSourceClient*> copy(_clients.begin(), _clients.end());
    for (auto* c : copy)
        if (c->client()) c->client()->close(true);  // triggers _handleDisconnect
}

void AsyncEventSource::send(const char* message, const char* event, uint32_t id, uint32_t reconnect) {
    std::string m = generateEventMessage(message, event, id, reconnect);
    for (auto* c : _clients)
        if (c->connected()) c->write(m.data(), m.size());
}

size_t AsyncEventSource::avgPacketsWaiting() const {
    if (_clients.empty()) return 0;
    size_t total = 0;
    for (auto* c : _clients) total += c->packetsWaiting();
    return total / _clients.size();
}

void AsyncEventSource::_addClient(AsyncEventSourceClient* client) {
    _clients.push_back(client);
    if (_connectcb) _connectcb(client);
}

void AsyncEventSource::_handleDisconnect(AsyncEventSourceClient* client) {
    _clients.remove(client);
    delete client;
}

bool AsyncEventSource::canHandle(AsyncWebServerRequest* request) {
    if (request->method() != HTTP_GET || !request->url().equals(_url) || !request->isExpectedRequestedConnType(RCT_EVENT)) return false;
    return true;
}

void AsyncEventSource::handleRequest(AsyncWebServerRequest* request) {
    if (_username.length() && _password.length() && !request->authenticate(_username.c_str(), _password.c_str()))
        return request->requestAuthentication();
    if (_authorizeConnectHandler && !_authorizeConnectHandler(request)) return request->send(401);
    request->send(new AsyncEventSourceResponse(this));
}

// ---------------------------------------------------------------------------
// AsyncEventSourceResponse
// ---------------------------------------------------------------------------
AsyncEventSourceResponse::AsyncEventSourceResponse(AsyncEventSource* server) : _server(server) {
    _code = 200;
    _contentType = "text/event-stream";
    _sendContentLength = false;
    addHeader("Cache-Control", "no-cache");
    addHeader("Connection", "keep-alive");
}

void AsyncEventSourceResponse::_respond(AsyncWebServerRequest* request) {
    String head = _assembleHead(request->version());
    request->client()->write(head.c_str(), head.length());
    _state = RESPONSE_WAIT_ACK;
}

size_t AsyncEventSourceResponse::_ack(AsyncWebServerRequest* request, size_t len, uint32_t) {
    if (len) new AsyncEventSourceClient(request, _server);  // deletes the request (and this response)
    return 0;
}
