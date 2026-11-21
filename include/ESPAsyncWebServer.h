// ESPAsyncWebServer emulation: AsyncWebServer on top of the emulated AsyncServer.
//
// Mirrors the public API of me-no-dev/ESPAsyncWebServer (routing, request and
// response objects, handlers, static files, body/upload handling, Basic and
// Digest authentication, rewrites, template processing). It is built on this
// project's own AsyncServer/AsyncClient, exactly like the real library sits on
// AsyncTCP, so everything runs inside AsyncEmu::poll() on the caller's thread.
//
// Behaviour carried over from the real library:
//  * One request per connection: every response carries "Connection: close"
//    and the connection is closed after the response has been sent.
//  * Unknown-length bodies use chunked encoding on HTTP/1.1.
//  * With no matching handler the catch-all (onNotFound) runs; by default it
//    sends an empty 404.
//  * A request that sends nothing for 3 seconds before a response starts is
//    closed (setRequestTimeout() is an emulation extension to change that).
//
// Emulation differences are listed in wiki/ESPAsyncWebServer.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "ESPAsyncTCP.h"
#include "FS.h"
#include "WString.h"

typedef enum {
    HTTP_GET = 0b00000001,
    HTTP_POST = 0b00000010,
    HTTP_DELETE = 0b00000100,
    HTTP_PUT = 0b00001000,
    HTTP_PATCH = 0b00010000,
    HTTP_HEAD = 0b00100000,
    HTTP_OPTIONS = 0b01000000,
    HTTP_ANY = 0b01111111,
} WebRequestMethod;

typedef uint8_t WebRequestMethodComposite;
typedef std::function<void(void)> ArDisconnectHandler;

// What kind of connection the request is asking for (used by WebSocket/SSE).
typedef enum { RCT_NOT_USED = -1, RCT_DEFAULT = 0, RCT_HTTP, RCT_WS, RCT_EVENT, RCT_MAX } RequestedConnectionType;

class AsyncWebServer;
class AsyncWebServerRequest;
class AsyncWebServerResponse;
class AsyncWebHandler;
class AsyncStaticWebHandler;
class AsyncCallbackWebHandler;
class AsyncWebRewrite;

#define RESPONSE_TRY_AGAIN 0xFFFFFFFF

// buffer, max length, bytes already produced -> bytes written (0 = end, RESPONSE_TRY_AGAIN = not yet)
typedef std::function<size_t(uint8_t*, size_t, size_t)> AwsResponseFiller;
// "%NAME%" placeholder -> replacement text
typedef std::function<String(const String&)> AwsTemplateProcessor;

typedef std::function<void(AsyncWebServerRequest*)> ArRequestHandlerFunction;
typedef std::function<void(AsyncWebServerRequest*, const String& filename, size_t index, uint8_t* data, size_t len, bool final)>
    ArUploadHandlerFunction;
typedef std::function<void(AsyncWebServerRequest*, uint8_t* data, size_t len, size_t index, size_t total)> ArBodyHandlerFunction;
typedef std::function<bool(AsyncWebServerRequest*)> ArRequestFilterFunction;

// Stand-ins for "which WiFi interface did the request arrive on". The desktop
// has one interface, so everything counts as the station side.
inline bool ON_STA_FILTER(AsyncWebServerRequest*) { return true; }
inline bool ON_AP_FILTER(AsyncWebServerRequest*) { return false; }

// ---------------------------------------------------------------------------
// Headers and parameters
// ---------------------------------------------------------------------------
class AsyncWebHeader {
public:
    AsyncWebHeader(const String& name, const String& value) : _name(name), _value(value) {}
    AsyncWebHeader(const String& data);  // "Name: value"
    const String& name() const { return _name; }
    const String& value() const { return _value; }
    String toString() const { return _name + ": " + _value + "\r\n"; }

private:
    String _name, _value;
};

class AsyncWebParameter {
public:
    AsyncWebParameter(const String& name, const String& value, bool form = false, bool file = false, size_t size = 0)
        : _name(name), _value(value), _size(size), _isForm(form), _isFile(file) {}
    const String& name() const { return _name; }
    const String& value() const { return _value; }
    size_t size() const { return _size; }
    bool isPost() const { return _isForm; }
    bool isFile() const { return _isFile; }

private:
    String _name, _value;
    size_t _size;
    bool _isForm, _isFile;
};

// Headers added to every response (e.g. CORS), as in the real library.
class DefaultHeaders {
public:
    static DefaultHeaders& Instance() { static DefaultHeaders inst; return inst; }
    void addHeader(const String& name, const String& value) { _headers.emplace_back(name, value); }
    void clear() { _headers.clear(); }
    const std::list<AsyncWebHeader>& headers() const { return _headers; }

private:
    std::list<AsyncWebHeader> _headers;
};

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------
typedef enum { RESPONSE_SETUP, RESPONSE_HEADERS, RESPONSE_CONTENT, RESPONSE_WAIT_ACK, RESPONSE_END, RESPONSE_FAILED } WebResponseState;

class AsyncWebServerResponse {
public:
    AsyncWebServerResponse();
    virtual ~AsyncWebServerResponse() {}

    void setCode(int code);
    int code() const { return _code; }
    void setContentLength(size_t len);
    void setContentType(const String& type);
    void addHeader(const String& name, const String& value);

    static const char* responseCodeToString(int code);

    // ---- internal engine (public as in the real library) ----
    String _assembleHead(uint8_t version);
    virtual bool _started() const { return _state > RESPONSE_SETUP; }
    virtual bool _finished() const { return _state > RESPONSE_WAIT_ACK; }
    virtual bool _failed() const { return _state == RESPONSE_FAILED; }
    virtual bool _sourceValid() const { return false; }
    virtual void _respond(AsyncWebServerRequest* request) = 0;
    virtual size_t _ack(AsyncWebServerRequest* request, size_t len, uint32_t time) = 0;

protected:
    int _code;
    std::list<AsyncWebHeader> _headers;
    String _contentType;
    size_t _contentLength;
    bool _sendContentLength;
    bool _chunked;
    WebResponseState _state;
};

// Base for everything that produces its body through _fillBuffer().
class AsyncAbstractResponse : public AsyncWebServerResponse {
public:
    AsyncAbstractResponse(AwsTemplateProcessor callback = nullptr);
    virtual ~AsyncAbstractResponse() {}
    void _respond(AsyncWebServerRequest* request) override;
    size_t _ack(AsyncWebServerRequest* request, size_t len, uint32_t time) override;
    bool _sourceValid() const override { return false; }

    // Returns bytes written, 0 at the end, or RESPONSE_TRY_AGAIN.
    virtual size_t _fillBuffer(uint8_t* buf, size_t maxLen) { (void)buf; (void)maxLen; return 0; }

protected:
    String _head;
    size_t _headWritten = 0;
    size_t _sentLength = 0;     // body bytes (after template processing) handed to the client
    size_t _writtenLength = 0;  // every byte handed to the client (head, chunk framing, body)
    size_t _ackedLength = 0;
    size_t _filledLength = 0;   // raw bytes produced by _fillBuffer
    bool _isHead = false;
    bool _bodyAllowed = true;
    bool _endSent = false;

    AwsTemplateProcessor _callback;
    std::string _raw;    // template mode: raw bytes not yet processed
    std::string _cache;  // template mode: processed bytes not yet sent
    bool _srcDone = false;

    size_t _fillProcessed(uint8_t* buf, size_t maxLen);
    void _pump(AsyncWebServerRequest* request);
};

// Fixed in-memory content.
class AsyncBasicResponse : public AsyncAbstractResponse {
public:
    AsyncBasicResponse(int code, const String& contentType = String(), const String& content = String());
    bool _sourceValid() const override { return true; }
    size_t _fillBuffer(uint8_t* buf, size_t maxLen) override;

private:
    String _content;
    size_t _pos = 0;
};

// Content from a filler callback; length may be 0 (unknown -> chunked).
class AsyncCallbackResponse : public AsyncAbstractResponse {
public:
    AsyncCallbackResponse(const String& contentType, size_t len, AwsResponseFiller callback, AwsTemplateProcessor templateCallback = nullptr);
    bool _sourceValid() const override { return true; }
    size_t _fillBuffer(uint8_t* buf, size_t maxLen) override;

private:
    AwsResponseFiller _content;
    size_t _filled = 0;
};

// Always chunked.
class AsyncChunkedResponse : public AsyncAbstractResponse {
public:
    AsyncChunkedResponse(const String& contentType, AwsResponseFiller callback, AwsTemplateProcessor templateCallback = nullptr);
    bool _sourceValid() const override { return true; }
    size_t _fillBuffer(uint8_t* buf, size_t maxLen) override;

private:
    AwsResponseFiller _content;
    size_t _filled = 0;
};

// Print-like response: write into it, then request->send(response).
class AsyncResponseStream : public AsyncAbstractResponse {
public:
    AsyncResponseStream(const String& contentType, size_t bufferSize = 1460);
    bool _sourceValid() const override { return true; }
    size_t _fillBuffer(uint8_t* buf, size_t maxLen) override;

    size_t write(const uint8_t* data, size_t len);
    size_t write(uint8_t b) { return write(&b, 1); }
    size_t print(const String& s) { return write(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
    size_t print(const char* s) { return s ? write(reinterpret_cast<const uint8_t*>(s), std::char_traits<char>::length(s)) : 0; }
    size_t print(int v) { return print(String(v)); }
    size_t print(unsigned v) { return print(String(v)); }
    size_t print(long v) { return print(String(v)); }
    size_t print(double v) { return print(String(v)); }
    size_t println(const String& s = String()) { size_t n = print(s); n += print("\r\n"); return n; }
    size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

private:
    std::string _buf;
    size_t _pos = 0;
};

// A file from an FS. A ".gz" path is sent with Content-Encoding: gzip.
class AsyncFileResponse : public AsyncAbstractResponse {
public:
    AsyncFileResponse(FS& fs, const String& path, const String& contentType = String(), bool download = false,
                      AwsTemplateProcessor callback = nullptr);
    AsyncFileResponse(File content, const String& path, const String& contentType = String(), bool download = false,
                      AwsTemplateProcessor callback = nullptr);
    bool _sourceValid() const override { return static_cast<bool>(_content); }
    size_t _fillBuffer(uint8_t* buf, size_t maxLen) override;

private:
    void _init(const String& path, const String& contentType, bool download);
    File _content;
};

// Constant data (PROGMEM on the device).
class AsyncProgmemResponse : public AsyncAbstractResponse {
public:
    AsyncProgmemResponse(int code, const String& contentType, const uint8_t* content, size_t len, AwsTemplateProcessor callback = nullptr);
    bool _sourceValid() const override { return true; }
    size_t _fillBuffer(uint8_t* buf, size_t maxLen) override;

private:
    const uint8_t* _data;
    size_t _pos = 0;
};

// ---------------------------------------------------------------------------
// Request
// ---------------------------------------------------------------------------
class AsyncWebServerRequest {
public:
    AsyncWebServerRequest(AsyncWebServer* server, AsyncClient* client);
    ~AsyncWebServerRequest();

    AsyncClient* client() { return _client; }
    uint8_t version() const { return _version; }  // 0 = HTTP/1.0, 1 = HTTP/1.1
    WebRequestMethodComposite method() const { return _method; }
    const String& url() const { return _url; }    // decoded path, without the query
    const String& host() const { return _host; }
    const String& contentType() const { return _contentType; }
    size_t contentLength() const { return _contentLength; }
    bool multipart() const { return _isMultipart; }
    const char* methodToString() const;
    const char* requestedConnTypeToString() const;
    RequestedConnectionType requestedConnType() const { return _reqconntype; }
    bool isExpectedRequestedConnType(RequestedConnectionType erct1, RequestedConnectionType erct2 = RCT_NOT_USED,
                                     RequestedConnectionType erct3 = RCT_NOT_USED) const;

    // ---- authentication ----
    bool authenticate(const char* username, const char* password, const char* realm = nullptr, bool passwordIsHash = false);
    bool authenticate(const char* hash);  // Basic: base64("user:password")
    void requestAuthentication(const char* realm = nullptr, bool isDigest = true);

    void setHandler(AsyncWebHandler* handler) { _handler = handler; }
    void addInterestingHeader(const String&) {}  // all headers are kept in the emulation
    void onDisconnect(ArDisconnectHandler fn) { _onDisconnectfn = std::move(fn); }

    // ---- responding ----
    void redirect(const String& url);
    void send(AsyncWebServerResponse* response);
    void send(int code, const String& contentType = String(), const String& content = String());
    void send(FS& fs, const String& path, const String& contentType = String(), bool download = false, AwsTemplateProcessor callback = nullptr);
    void send(File content, const String& path, const String& contentType = String(), bool download = false, AwsTemplateProcessor callback = nullptr);
    void send(const String& contentType, size_t len, AwsResponseFiller callback, AwsTemplateProcessor templateCallback = nullptr);
    void sendChunked(const String& contentType, AwsResponseFiller callback, AwsTemplateProcessor templateCallback = nullptr);
    void send_P(int code, const String& contentType, const uint8_t* content, size_t len, AwsTemplateProcessor callback = nullptr);
    void send_P(int code, const String& contentType, const char* content, AwsTemplateProcessor callback = nullptr);

    AsyncWebServerResponse* beginResponse(int code, const String& contentType = String(), const String& content = String());
    AsyncWebServerResponse* beginResponse(FS& fs, const String& path, const String& contentType = String(), bool download = false,
                                          AwsTemplateProcessor callback = nullptr);
    AsyncWebServerResponse* beginResponse(File content, const String& path, const String& contentType = String(), bool download = false,
                                          AwsTemplateProcessor callback = nullptr);
    AsyncWebServerResponse* beginResponse(const String& contentType, size_t len, AwsResponseFiller callback,
                                          AwsTemplateProcessor templateCallback = nullptr);
    AsyncWebServerResponse* beginChunkedResponse(const String& contentType, AwsResponseFiller callback, AwsTemplateProcessor templateCallback = nullptr);
    AsyncResponseStream* beginResponseStream(const String& contentType, size_t bufferSize = 1460);
    AsyncWebServerResponse* beginResponse_P(int code, const String& contentType, const uint8_t* content, size_t len,
                                            AwsTemplateProcessor callback = nullptr);
    AsyncWebServerResponse* beginResponse_P(int code, const String& contentType, const char* content, AwsTemplateProcessor callback = nullptr);

    // ---- headers ----
    size_t headers() const { return _headers.size(); }
    bool hasHeader(const String& name) const;
    AsyncWebHeader* getHeader(const String& name) const;
    AsyncWebHeader* getHeader(size_t num) const;
    const String& header(const char* name) const;
    const String& header(const String& name) const { return header(name.c_str()); }
    const String& header(size_t num) const;
    const String& headerName(size_t num) const;

    // ---- parameters (query string, form body, multipart) ----
    size_t params() const { return _params.size(); }
    bool hasParam(const String& name, bool post = false, bool file = false) const;
    AsyncWebParameter* getParam(const String& name, bool post = false, bool file = false) const;
    AsyncWebParameter* getParam(size_t num) const;
    size_t args() const { return params(); }
    const String& arg(const String& name) const;  // GET and POST, not files
    const String& arg(size_t i) const;
    const String& argName(size_t i) const;
    bool hasArg(const char* name) const;
    const String& pathArg(size_t i) const;  // regex capture groups of a "^...$" route

    String urlDecode(const String& text) const;

    void* _tempObject = nullptr;  // user data, free()d with the request (as in the real library)

    // ---- internal (parser and engine) ----
    void _onPoll();
    void _onAck(size_t len, uint32_t time);
    void _onError(int8_t error);
    void _onTimeout(uint32_t time);
    void _onDisconnect();
    void _onData(void* buf, size_t len);
    void _addParam(AsyncWebParameter* p);
    void _addPathParam(const char* param);
    void _addGetParams(const String& params);  // "a=1&b=2", used by rewrites
    void _finishResponse();  // response fully sent: close the connection

    AsyncWebServer* _server;
    String _url;  // writable so rewrites can change it

private:
    enum ParseState { PARSE_REQ_START, PARSE_REQ_HEADERS, PARSE_REQ_BODY, PARSE_REQ_END, PARSE_REQ_FAIL };

    AsyncClient* _client;
    AsyncWebServerResponse* _response = nullptr;
    AsyncWebHandler* _handler = nullptr;
    ArDisconnectHandler _onDisconnectfn;

    ParseState _parseState = PARSE_REQ_START;
    std::string _line;
    uint8_t _version = 0;
    WebRequestMethodComposite _method = HTTP_ANY;
    String _host, _contentType, _boundary, _authorization;
    size_t _contentLength = 0;
    size_t _parsedLength = 0;
    bool _isMultipart = false, _isPlainPost = false, _expectingContinue = false;
    RequestedConnectionType _reqconntype = RCT_HTTP;

    std::deque<AsyncWebHeader> _headers;
    std::deque<AsyncWebParameter> _params;
    std::vector<String> _pathParams;
    std::string _bodyBuf;  // urlencoded bodies

    // multipart state machine
    struct Multipart {
        enum St { PREAMBLE, AFTER_BOUNDARY, HEADERS, BODY, DONE } st = PREAMBLE;
        std::string boundary, delim, buf, value;
        String name, filename;
        bool isFile = false;
        size_t itemSize = 0;
    } _mp;

    void _parseLine();
    void _parseRequestLine(const std::string& line);
    void _parseHeaderLine(const std::string& line);
    void _headersComplete();
    void _feedBody(const uint8_t* p, size_t n);
    void _mpFeed(const uint8_t* p, size_t n);
    void _mpStartItem(const std::string& headerBlock);
    void _mpEmit(const char* data, size_t len);
    void _mpFinishItem();
    void _parseQuery(const std::string& q, bool post);
    void _handleRequest();
    void _fail(int code);
    bool _bodyHandlerActive() const;
};

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
class AsyncWebRewrite {
public:
    AsyncWebRewrite(const char* from, const char* to);
    virtual ~AsyncWebRewrite() {}
    AsyncWebRewrite& setFilter(ArRequestFilterFunction fn) { _filter = std::move(fn); return *this; }
    bool filter(AsyncWebServerRequest* request) const { return !_filter || _filter(request); }
    const String& from() const { return _from; }
    const String& toUrl() const { return _toUrl; }
    const String& params() const { return _params; }
    virtual bool match(AsyncWebServerRequest* request) { return _from == request->url() && filter(request); }

protected:
    String _from, _toUrl, _params;
    ArRequestFilterFunction _filter;
};

class AsyncWebHandler {
public:
    virtual ~AsyncWebHandler() {}
    AsyncWebHandler& setFilter(ArRequestFilterFunction fn) { _filter = std::move(fn); return *this; }
    AsyncWebHandler& setAuthentication(const char* username, const char* password) {
        _username = username ? username : "";
        _password = password ? password : "";
        return *this;
    }
    bool filter(AsyncWebServerRequest* request) { return !_filter || _filter(request); }

    virtual bool canHandle(AsyncWebServerRequest* request) { (void)request; return false; }
    virtual void handleRequest(AsyncWebServerRequest* request) { request->send(404); }
    virtual void handleUpload(AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool) {}
    virtual void handleBody(AsyncWebServerRequest*, uint8_t*, size_t, size_t, size_t) {}
    virtual bool isRequestHandlerTrivial() { return true; }

protected:
    ArRequestFilterFunction _filter;
    String _username, _password;
};

class AsyncCallbackWebHandler : public AsyncWebHandler {
public:
    AsyncCallbackWebHandler() : _uri(), _method(HTTP_ANY) {}
    void setUri(const String& uri);
    void setMethod(WebRequestMethodComposite method) { _method = method; }
    void onRequest(ArRequestHandlerFunction fn) { _onRequest = std::move(fn); }
    void onUpload(ArUploadHandlerFunction fn) { _onUpload = std::move(fn); }
    void onBody(ArBodyHandlerFunction fn) { _onBody = std::move(fn); }

    bool canHandle(AsyncWebServerRequest* request) override;
    void handleRequest(AsyncWebServerRequest* request) override;
    void handleUpload(AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final) override;
    void handleBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) override;
    bool isRequestHandlerTrivial() override { return !(_onUpload || _onBody); }

private:
    String _uri;
    WebRequestMethodComposite _method;
    ArRequestHandlerFunction _onRequest;
    ArUploadHandlerFunction _onUpload;
    ArBodyHandlerFunction _onBody;
    bool _isRegex = false;
    std::regex _uriRegex;
};

// serveStatic(): files from an FS under a URL prefix, with default file,
// gzip variants, Cache-Control, ETag / Last-Modified and 304 handling.
class AsyncStaticWebHandler : public AsyncWebHandler {
public:
    AsyncStaticWebHandler(const char* uri, FS& fs, const char* path, const char* cache_control);
    bool canHandle(AsyncWebServerRequest* request) override;
    void handleRequest(AsyncWebServerRequest* request) override;

    AsyncStaticWebHandler& setIsDir(bool isDir) { _isDir = isDir; return *this; }
    AsyncStaticWebHandler& setDefaultFile(const char* filename) { _defaultFile = filename; return *this; }
    AsyncStaticWebHandler& setCacheControl(const char* cache_control) { _cache_control = cache_control ? cache_control : ""; return *this; }
    AsyncStaticWebHandler& setLastModified(const char* last_modified) { _last_modified = last_modified ? last_modified : ""; return *this; }
    AsyncStaticWebHandler& setLastModified(time_t last_modified);
    AsyncStaticWebHandler& setLastModified();  // use each file's modification time
    AsyncStaticWebHandler& setTemplateProcessor(AwsTemplateProcessor newCallback) { _callback = std::move(newCallback); return *this; }

private:
    bool _getFile(AsyncWebServerRequest* request, String& outPath, bool& gz);

    FS& _fs;
    String _uri, _path, _defaultFile, _cache_control, _last_modified;
    bool _isDir;
    bool _useFileMtime = false;
    AwsTemplateProcessor _callback;
};

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
class AsyncWebServer {
public:
    AsyncWebServer(uint16_t port);
    ~AsyncWebServer();
    AsyncWebServer(const AsyncWebServer&) = delete;
    AsyncWebServer& operator=(const AsyncWebServer&) = delete;

    void begin();
    void end();
    uint16_t port();  // extension: bound port (AsyncWebServer(0) picks a free one)

    // addRewrite()/addHandler() do NOT take ownership (emulation difference): sketches
    // routinely pass the address of a global (`server.addHandler(&ws)`), and on the
    // desktop globals are destroyed at exit, so deleting them here would crash.
    // Objects created by the server itself (rewrite(), on(), serveStatic()) are owned.
    AsyncWebRewrite& addRewrite(AsyncWebRewrite* rewrite);
    bool removeRewrite(AsyncWebRewrite* rewrite);
    AsyncWebRewrite& rewrite(const char* from, const char* to);

    AsyncWebHandler& addHandler(AsyncWebHandler* handler);
    bool removeHandler(AsyncWebHandler* handler);

    AsyncCallbackWebHandler& on(const char* uri, ArRequestHandlerFunction onRequest);
    AsyncCallbackWebHandler& on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest);
    AsyncCallbackWebHandler& on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest,
                                ArUploadHandlerFunction onUpload);
    AsyncCallbackWebHandler& on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest,
                                ArUploadHandlerFunction onUpload, ArBodyHandlerFunction onBody);

    AsyncStaticWebHandler& serveStatic(const char* uri, FS& fs, const char* path, const char* cache_control = nullptr);

    void onNotFound(ArRequestHandlerFunction fn);   // catch-all
    void onFileUpload(ArUploadHandlerFunction fn);  // uploads no route claimed
    void onRequestBody(ArBodyHandlerFunction fn);   // raw bodies no route claimed
    void reset();                                   // drop all handlers and rewrites

    // Emulation extension: seconds of silence before an unanswered request is
    // closed (real library: 3). 0 disables the timeout.
    void setRequestTimeout(uint32_t seconds) { _requestTimeout = seconds; }
    uint32_t requestTimeout() const { return _requestTimeout; }

    void _trackRequest(AsyncWebServerRequest* request) { _requests.insert(request); }
    void _handleDisconnect(AsyncWebServerRequest* request);
    void _attachHandler(AsyncWebServerRequest* request);
    void _rewriteRequest(AsyncWebServerRequest* request);

private:
    AsyncServer _server;
    template <class T>
    struct Slot {
        T* ptr;
        bool owned;
    };
    std::list<Slot<AsyncWebRewrite>> _rewrites;
    std::list<Slot<AsyncWebHandler>> _handlers;
    AsyncWebHandler& _addOwnedHandler(AsyncWebHandler* handler);
    std::unique_ptr<AsyncCallbackWebHandler> _catchAllHandler;
    std::set<AsyncWebServerRequest*> _requests;  // live requests, closed on destruction
    uint32_t _requestTimeout = 3;
};

// The real library pulls these in from ESPAsyncWebServer.h as well.
#include "AsyncEventSource.h"
#include "AsyncWebSocket.h"
