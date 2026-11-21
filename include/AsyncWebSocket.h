// WebSocket server (AsyncWebSocket), API-compatible with ESPAsyncWebServer.
//
//   AsyncWebSocket ws("/ws");
//   ws.onEvent([](AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType type, void* arg, uint8_t* data, size_t len) {
//       if (type == WS_EVT_DATA) {
//           AwsFrameInfo* info = static_cast<AwsFrameInfo*>(arg);
//           if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) c->text("echo");
//       }
//   });
//   server.addHandler(&ws);
//
// Implements RFC 6455 framing: handshake, text/binary/continuation frames
// (delivered chunk by chunk, with AwsFrameInfo describing each frame),
// ping/pong, close handshake. Server frames are unmasked; client frames must
// be masked. Outgoing messages are queued (at most 32 per client).
#pragma once
#include <cstdarg>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "ESPAsyncWebServer.h"

#define DEFAULT_MAX_WS_CLIENTS 4
#define WS_MAX_QUEUED_MESSAGES 32

class AsyncWebSocket;
class AsyncWebSocketClient;

typedef enum { WS_DISCONNECTED, WS_CONNECTED, WS_DISCONNECTING } AwsClientStatus;
typedef enum { WS_CONTINUATION, WS_TEXT, WS_BINARY, WS_DISCONNECT = 0x08, WS_PING, WS_PONG } AwsFrameType;
typedef enum { WS_EVT_CONNECT, WS_EVT_DISCONNECT, WS_EVT_PONG, WS_EVT_ERROR, WS_EVT_DATA } AwsEventType;

typedef struct {
    uint8_t message_opcode;  // opcode of the first frame of the message
    uint32_t num;            // frame number within the message
    uint8_t final;           // FIN bit of this frame
    uint8_t masked;
    uint8_t opcode;          // opcode of this frame (WS_CONTINUATION for continuations)
    uint64_t len;            // payload length of this frame
    uint8_t mask[4];
    uint64_t index;          // offset of the delivered chunk within the frame payload
} AwsFrameInfo;

typedef std::function<void(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len)>
    AwsEventHandler;

// Bytes to broadcast. The server takes ownership once you pass it to text()/binary().
class AsyncWebSocketMessageBuffer {
public:
    AsyncWebSocketMessageBuffer() {}
    AsyncWebSocketMessageBuffer(size_t size) : _buf(size) {}
    AsyncWebSocketMessageBuffer(const uint8_t* data, size_t size) : _buf(data, data + size) {}
    uint8_t* get() { return _buf.data(); }
    size_t length() const { return _buf.size(); }
    bool reserve(size_t size) { _buf.resize(size); return true; }
    void lock() {}
    void unlock() {}

private:
    std::vector<uint8_t> _buf;
};

class AsyncWebSocketClient {
public:
    AsyncWebSocketClient(AsyncWebServerRequest* request, AsyncWebSocket* server);
    ~AsyncWebSocketClient();

    AsyncClient* client() { return _client; }
    AsyncWebSocket* server() { return _server; }
    uint32_t id() const { return _clientId; }
    AwsClientStatus status() const { return _status; }

    bool canSend() const { return _status == WS_CONNECTED && _queue.size() < WS_MAX_QUEUED_MESSAGES; }
    bool queueIsFull() const { return _queue.size() >= WS_MAX_QUEUED_MESSAGES; }
    size_t queueLen() const { return _queue.size(); }

    void close(uint16_t code = 0, const char* message = nullptr);
    void ping(uint8_t* data = nullptr, size_t len = 0);

    void text(const char* message, size_t len);
    void text(const char* message);
    void text(uint8_t* message, size_t len);
    void text(const String& message);
    void text(AsyncWebSocketMessageBuffer* buffer);
    void binary(const char* message, size_t len);
    void binary(const char* message);
    void binary(uint8_t* message, size_t len);
    void binary(const String& message);
    void binary(AsyncWebSocketMessageBuffer* buffer);

    size_t printf(const char* format, ...) __attribute__((format(printf, 2, 3)));

    IPAddress remoteIP() { return _client ? _client->remoteIP() : IPAddress(); }
    uint16_t remotePort() { return _client ? _client->remotePort() : 0; }

    void keepAlivePeriod(uint16_t seconds) { _keepAlive = seconds; }
    uint16_t keepAlivePeriod() const { return _keepAlive; }

    // internal
    void _onAck(size_t len, uint32_t time);
    void _onError(int8_t err);
    void _onPoll();
    void _onTimeout(uint32_t time);
    void _onDisconnect();
    void _onData(void* buf, size_t len);

private:
    void _queueFrame(uint8_t opcode, const uint8_t* data, size_t len, bool control);
    void _runQueue();
    void _handleControl();

    AsyncClient* _client;
    AsyncWebSocket* _server;
    uint32_t _clientId;
    AwsClientStatus _status = WS_CONNECTED;

    std::deque<std::string> _queue;
    size_t _offset = 0;
    bool _closeQueued = false;   // close the TCP connection as soon as the queue is flushed
    uint32_t _closeDeadline = 0;  // server-initiated close: when to stop waiting for the peer
    bool* _alive = nullptr;       // set by _onData so a handler destroying the client is noticed
    uint16_t _keepAlive = 0;
    uint32_t _lastActivity = 0;

    // incoming frame parser
    std::string _hdr;       // header bytes collected so far
    bool _inPayload = false;
    AwsFrameInfo _info{};
    uint64_t _received = 0;  // payload bytes of the current frame consumed
    uint32_t _frameNum = 0;
    uint8_t _messageOpcode = 0;
    std::string _ctrl;       // payload of the current control frame
};

class AsyncWebSocket : public AsyncWebHandler {
public:
    AsyncWebSocket(const String& url) : _url(url) {}
    ~AsyncWebSocket();

    const char* url() const { return _url.c_str(); }
    void enable(bool e) { _enabled = e; }
    bool enabled() const { return _enabled; }

    size_t count() const { return _clients.size(); }
    AsyncWebSocketClient* client(uint32_t id);
    bool hasClient(uint32_t id) { return client(id) != nullptr; }

    void close(uint32_t id, uint16_t code = 0, const char* message = nullptr);
    void closeAll(uint16_t code = 0, const char* message = nullptr);
    void cleanupClients(uint16_t maxClients = DEFAULT_MAX_WS_CLIENTS);  // closes the oldest if there are too many

    void ping(uint32_t id, uint8_t* data = nullptr, size_t len = 0);
    void pingAll(uint8_t* data = nullptr, size_t len = 0);

    void text(uint32_t id, const char* message, size_t len);
    void text(uint32_t id, const char* message);
    void text(uint32_t id, const String& message);
    void text(uint32_t id, AsyncWebSocketMessageBuffer* buffer);
    void textAll(const char* message, size_t len);
    void textAll(const char* message);
    void textAll(const String& message);
    void textAll(AsyncWebSocketMessageBuffer* buffer);
    void binary(uint32_t id, const char* message, size_t len);
    void binary(uint32_t id, const char* message);
    void binary(uint32_t id, const String& message);
    void binary(uint32_t id, AsyncWebSocketMessageBuffer* buffer);
    void binaryAll(const char* message, size_t len);
    void binaryAll(const char* message);
    void binaryAll(const String& message);
    void binaryAll(AsyncWebSocketMessageBuffer* buffer);

    size_t printf(uint32_t id, const char* format, ...) __attribute__((format(printf, 3, 4)));
    size_t printfAll(const char* format, ...) __attribute__((format(printf, 2, 3)));

    AsyncWebSocketMessageBuffer* makeBuffer(size_t size = 0) { return new AsyncWebSocketMessageBuffer(size); }
    AsyncWebSocketMessageBuffer* makeBuffer(const uint8_t* data, size_t size) { return new AsyncWebSocketMessageBuffer(data, size); }

    void onEvent(AwsEventHandler handler) { _eventHandler = std::move(handler); }

    bool canHandle(AsyncWebServerRequest* request) override;
    void handleRequest(AsyncWebServerRequest* request) override;

    // internal
    void _addClient(AsyncWebSocketClient* client);
    void _handleDisconnect(AsyncWebSocketClient* client);
    void _handleEvent(AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len);
    uint32_t _getNextId() { return _nextId++; }

private:
    String _url;
    std::list<AsyncWebSocketClient*> _clients;
    uint32_t _nextId = 1;
    AwsEventHandler _eventHandler;
    bool _enabled = true;
};

// 101 Switching Protocols; hands the connection to an AsyncWebSocketClient once acked.
class AsyncWebSocketResponse : public AsyncWebServerResponse {
public:
    AsyncWebSocketResponse(const String& key, AsyncWebSocket* server);
    void _respond(AsyncWebServerRequest* request) override;
    size_t _ack(AsyncWebServerRequest* request, size_t len, uint32_t time) override;
    bool _sourceValid() const override { return true; }

private:
    AsyncWebSocket* _server;
    String _content;
};
