// Server-Sent Events (AsyncEventSource), API-compatible with ESPAsyncWebServer.
//
//   AsyncEventSource events("/events");
//   events.onConnect([](AsyncEventSourceClient* c) { c->send("hello", nullptr, 1, 3000); });
//   server.addHandler(&events);
//   ...
//   events.send("temperature=21.5", "sensor");
//
// A request is treated as an event-stream connection when it is a GET to the
// handler's URL carrying "Accept: text/event-stream". The connection then stays
// open and messages are queued (at most 32 per client) and written as the
// client's send buffer drains.
#pragma once
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <string>

#include "ESPAsyncWebServer.h"

#define SSE_MAX_QUEUED_MESSAGES 32

class AsyncEventSource;
class AsyncEventSourceClient;

typedef std::function<void(AsyncEventSourceClient* client)> ArEventHandlerFunction;
typedef std::function<bool(AsyncWebServerRequest* request)> ArAuthorizeConnectHandler;

class AsyncEventSourceClient {
public:
    AsyncEventSourceClient(AsyncWebServerRequest* request, AsyncEventSource* server);
    ~AsyncEventSourceClient();

    AsyncClient* client() { return _client; }
    void close();
    void write(const char* message, size_t len);
    void send(const char* message, const char* event = nullptr, uint32_t id = 0, uint32_t reconnect = 0);
    bool connected() const { return _client && _client->connected(); }
    uint32_t lastId() const { return _lastId; }
    size_t packetsWaiting() const { return _queue.size(); }

    // internal
    void _onAck(size_t len, uint32_t time);
    void _onPoll();
    void _onTimeout(uint32_t time);
    void _onDisconnect();

private:
    void _runQueue();

    AsyncClient* _client;
    AsyncEventSource* _server;
    uint32_t _lastId = 0;
    std::deque<std::string> _queue;
    size_t _offset = 0;  // bytes of _queue.front() already written
};

class AsyncEventSource : public AsyncWebHandler {
public:
    AsyncEventSource(const String& url) : _url(url) {}
    ~AsyncEventSource();

    const char* url() const { return _url.c_str(); }
    void close();  // disconnect every client
    void onConnect(ArEventHandlerFunction cb) { _connectcb = std::move(cb); }
    void authorizeConnect(ArAuthorizeConnectHandler cb) { _authorizeConnectHandler = std::move(cb); }
    void send(const char* message, const char* event = nullptr, uint32_t id = 0, uint32_t reconnect = 0);
    size_t count() const { return _clients.size(); }
    size_t avgPacketsWaiting() const;

    bool canHandle(AsyncWebServerRequest* request) override;
    void handleRequest(AsyncWebServerRequest* request) override;

    // internal
    void _addClient(AsyncEventSourceClient* client);
    void _handleDisconnect(AsyncEventSourceClient* client);

private:
    String _url;
    std::list<AsyncEventSourceClient*> _clients;
    ArEventHandlerFunction _connectcb;
    ArAuthorizeConnectHandler _authorizeConnectHandler;
};

// Hands the connection over to an AsyncEventSourceClient once the headers are acked.
class AsyncEventSourceResponse : public AsyncWebServerResponse {
public:
    AsyncEventSourceResponse(AsyncEventSource* server);
    void _respond(AsyncWebServerRequest* request) override;
    size_t _ack(AsyncWebServerRequest* request, size_t len, uint32_t time) override;
    bool _sourceValid() const override { return true; }

private:
    AsyncEventSource* _server;
};
