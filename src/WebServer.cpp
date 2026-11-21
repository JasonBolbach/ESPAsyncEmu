// AsyncWebServer, AsyncCallbackWebHandler, AsyncStaticWebHandler, AsyncWebRewrite.
#include <cstdio>

#include "ESPAsyncWebServer.h"
#include "WebUtil.h"

using namespace AsyncEmu::web;

// ---------------------------------------------------------------------------
// AsyncWebRewrite
// ---------------------------------------------------------------------------
AsyncWebRewrite::AsyncWebRewrite(const char* from, const char* to) : _from(from), _toUrl(to) {
    int index = _toUrl.indexOf('?');
    if (index > 0) {
        _params = _toUrl.substring(static_cast<size_t>(index) + 1);
        _toUrl = _toUrl.substring(0, static_cast<size_t>(index));
    }
}

// ---------------------------------------------------------------------------
// AsyncCallbackWebHandler
// ---------------------------------------------------------------------------
void AsyncCallbackWebHandler::setUri(const String& uri) {
    _uri = uri;
    _isRegex = uri.length() > 1 && uri.startsWith("^") && uri.endsWith("$");
    if (_isRegex) _uriRegex = std::regex(uri.c_str());
}

bool AsyncCallbackWebHandler::canHandle(AsyncWebServerRequest* request) {
    if (!_onRequest) return false;
    if (!(_method & request->method())) return false;

    if (_isRegex) {
        std::smatch m;
        std::string u = request->url();
        if (!std::regex_search(u, m, _uriRegex)) return false;
        for (size_t i = 1; i < m.size(); ++i) request->_addPathParam(m[i].str().c_str());
    } else if (_uri.length() && _uri.endsWith("*")) {
        String prefix = _uri.substring(0, _uri.length() - 1);
        if (!request->url().startsWith(prefix)) return false;
    } else if (_uri.length() && (_uri != request->url() && !request->url().startsWith(_uri + "/"))) {
        return false;
    }
    return true;
}

void AsyncCallbackWebHandler::handleRequest(AsyncWebServerRequest* request) {
    if (_username.length() && _password.length() && !request->authenticate(_username.c_str(), _password.c_str()))
        return request->requestAuthentication();
    if (_onRequest) _onRequest(request);
    else request->send(500);
}

void AsyncCallbackWebHandler::handleUpload(AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
    if (_onUpload) _onUpload(request, filename, index, data, len, final);
}

void AsyncCallbackWebHandler::handleBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (_onBody) _onBody(request, data, len, index, total);
}

// ---------------------------------------------------------------------------
// AsyncStaticWebHandler
// ---------------------------------------------------------------------------
AsyncStaticWebHandler::AsyncStaticWebHandler(const char* uri, FS& fs, const char* path, const char* cache_control)
    : _fs(fs), _uri(uri ? uri : ""), _path(path ? path : ""), _defaultFile("index.htm"), _cache_control(cache_control ? cache_control : "") {
    if (_uri.length() == 0 || _uri[0] != '/') _uri = "/" + _uri;
    if (_path.length() == 0 || _path[0] != '/') _path = "/" + _path;
    // A trailing '/' on the path hints that it is a directory.
    _isDir = _path[_path.length() - 1] == '/';
    // Strip trailing '/' so default-file handling works; the root becomes "".
    if (_uri[_uri.length() - 1] == '/') _uri = _uri.substring(0, _uri.length() - 1);
    if (_path[_path.length() - 1] == '/') _path = _path.substring(0, _path.length() - 1);
}

AsyncStaticWebHandler& AsyncStaticWebHandler::setLastModified(time_t last_modified) {
    _last_modified = httpDate(last_modified);
    return *this;
}

AsyncStaticWebHandler& AsyncStaticWebHandler::setLastModified() {
    _useFileMtime = true;
    return *this;
}

// Templates only make sense for text; running them over binaries would corrupt any
// "%...%" byte sequence (the real library has no such guard).
static bool isTextual(const String& mime) {
    return mime.startsWith("text/") || mime == "application/javascript" || mime == "application/json" ||
           mime == "application/xml" || mime == "image/svg+xml";
}

static bool isFile(FS& fs, const String& path) {
    File f = fs.open(path, "r");
    return f && !f.isDirectory();
}

bool AsyncStaticWebHandler::_getFile(AsyncWebServerRequest* request, String& outPath, bool& gz) {
    auto fileExists = [&](const String& p) {
        gz = false;
        if (isFile(_fs, p)) { outPath = p; return true; }
        if (isFile(_fs, p + ".gz")) { outPath = p + ".gz"; gz = true; return true; }
        return false;
    };

    String path = request->url().substring(_uri.length());
    bool canSkipFileCheck = (_isDir && path.length() == 0) || (path.length() && path[path.length() - 1] == '/');
    path = _path + path;

    if (!canSkipFileCheck && fileExists(path)) return true;

    // Directory (or missing file): try the default file inside it.
    if (path.length() == 0 || path[path.length() - 1] != '/') path += "/";
    path += _defaultFile;
    return fileExists(path);
}

bool AsyncStaticWebHandler::canHandle(AsyncWebServerRequest* request) {
    if (request->method() != HTTP_GET || !request->url().startsWith(_uri) ||
        !request->isExpectedRequestedConnType(RCT_DEFAULT, RCT_HTTP))
        return false;
    String path;
    bool gz;
    return _getFile(request, path, gz);
}

void AsyncStaticWebHandler::handleRequest(AsyncWebServerRequest* request) {
    if (_username.length() && _password.length() && !request->authenticate(_username.c_str(), _password.c_str()))
        return request->requestAuthentication();

    String path;
    bool gz;
    if (!_getFile(request, path, gz)) { request->send(404); return; }

    File f = _fs.open(path, "r");
    time_t mtime = f.getLastWrite();
    char etagBuf[48];
    std::snprintf(etagBuf, sizeof etagBuf, "\"%zx-%llx\"", f.size(), static_cast<unsigned long long>(mtime));
    String etag = etagBuf;
    f.close();

    String lastModified = _last_modified.length() ? _last_modified : (_useFileMtime ? String(httpDate(mtime)) : String());
    String basePath = gz ? path.substring(0, path.length() - 3) : path;
    bool templated = _callback && isTextual(mimeTypeForPath(basePath));

    // Conditional request?
    bool notModified = false;
    if (!templated) {
        if (request->hasHeader("If-None-Match")) notModified = request->header("If-None-Match") == etag;
        else if (lastModified.length() && request->hasHeader("If-Modified-Since")) notModified = request->header("If-Modified-Since") == lastModified;
    }

    auto decorate = [&](AsyncWebServerResponse* r) {
        if (!templated) r->addHeader("ETag", etag);
        if (lastModified.length()) r->addHeader("Last-Modified", lastModified);
        if (_cache_control.length()) r->addHeader("Cache-Control", _cache_control);
    };

    if (notModified) {
        AsyncWebServerResponse* r = request->beginResponse(304);
        decorate(r);
        request->send(r);
        return;
    }

    AsyncWebServerResponse* response = new AsyncFileResponse(_fs, path, String(), false, templated ? _callback : nullptr);
    decorate(response);
    request->send(response);
}

// ---------------------------------------------------------------------------
// AsyncWebServer
// ---------------------------------------------------------------------------
AsyncWebServer::AsyncWebServer(uint16_t port) : _server(port), _catchAllHandler(new AsyncCallbackWebHandler()) {
    _catchAllHandler->onRequest([](AsyncWebServerRequest* request) { request->send(404); });
}

AsyncWebServer::~AsyncWebServer() {
    _server.end();
    // Close live connections; each request deletes itself through _handleDisconnect().
    std::vector<AsyncWebServerRequest*> live(_requests.begin(), _requests.end());
    for (auto* r : live)
        if (_requests.count(r) && r->client()) r->client()->close(true);
    reset();
}

void AsyncWebServer::begin() {
    _server.onClient(
        [](void* s, AsyncClient* c) {
            if (!c) return;
            auto* self = static_cast<AsyncWebServer*>(s);
            c->setRxTimeout(self->_requestTimeout);
            new AsyncWebServerRequest(self, c);  // owns itself until the client disconnects
        },
        this);
    _server.begin();
}

void AsyncWebServer::end() { _server.end(); }
uint16_t AsyncWebServer::port() { return _server.port(); }

AsyncWebRewrite& AsyncWebServer::addRewrite(AsyncWebRewrite* rewrite) {
    _rewrites.push_back({rewrite, false});
    return *rewrite;
}

bool AsyncWebServer::removeRewrite(AsyncWebRewrite* rewrite) {
    for (auto it = _rewrites.begin(); it != _rewrites.end(); ++it)
        if (it->ptr == rewrite) {
            if (it->owned) delete it->ptr;
            _rewrites.erase(it);
            return true;
        }
    return false;
}

AsyncWebRewrite& AsyncWebServer::rewrite(const char* from, const char* to) {
    auto* r = new AsyncWebRewrite(from, to);
    _rewrites.push_back({r, true});
    return *r;
}

AsyncWebHandler& AsyncWebServer::addHandler(AsyncWebHandler* handler) {
    _handlers.push_back({handler, false});
    return *handler;
}

AsyncWebHandler& AsyncWebServer::_addOwnedHandler(AsyncWebHandler* handler) {
    _handlers.push_back({handler, true});
    return *handler;
}

bool AsyncWebServer::removeHandler(AsyncWebHandler* handler) {
    for (auto it = _handlers.begin(); it != _handlers.end(); ++it)
        if (it->ptr == handler) {
            if (it->owned) delete it->ptr;
            _handlers.erase(it);
            return true;
        }
    return false;
}

AsyncCallbackWebHandler& AsyncWebServer::on(const char* uri, ArRequestHandlerFunction onRequest) {
    return on(uri, HTTP_ANY, std::move(onRequest), nullptr, nullptr);
}
AsyncCallbackWebHandler& AsyncWebServer::on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest) {
    return on(uri, method, std::move(onRequest), nullptr, nullptr);
}
AsyncCallbackWebHandler& AsyncWebServer::on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest,
                                            ArUploadHandlerFunction onUpload) {
    return on(uri, method, std::move(onRequest), std::move(onUpload), nullptr);
}
AsyncCallbackWebHandler& AsyncWebServer::on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction onRequest,
                                            ArUploadHandlerFunction onUpload, ArBodyHandlerFunction onBody) {
    auto* handler = new AsyncCallbackWebHandler();
    handler->setUri(uri);
    handler->setMethod(method);
    handler->onRequest(std::move(onRequest));
    handler->onUpload(std::move(onUpload));
    handler->onBody(std::move(onBody));
    _addOwnedHandler(handler);
    return *handler;
}

AsyncStaticWebHandler& AsyncWebServer::serveStatic(const char* uri, FS& fs, const char* path, const char* cache_control) {
    auto* handler = new AsyncStaticWebHandler(uri, fs, path, cache_control);
    _addOwnedHandler(handler);
    return *handler;
}

void AsyncWebServer::onNotFound(ArRequestHandlerFunction fn) { _catchAllHandler->onRequest(std::move(fn)); }
void AsyncWebServer::onFileUpload(ArUploadHandlerFunction fn) { _catchAllHandler->onUpload(std::move(fn)); }
void AsyncWebServer::onRequestBody(ArBodyHandlerFunction fn) { _catchAllHandler->onBody(std::move(fn)); }

void AsyncWebServer::reset() {
    for (auto& r : _rewrites) if (r.owned) delete r.ptr;
    for (auto& h : _handlers) if (h.owned) delete h.ptr;
    _rewrites.clear();
    _handlers.clear();
    _catchAllHandler->onRequest([](AsyncWebServerRequest* request) { request->send(404); });
    _catchAllHandler->onUpload(nullptr);
    _catchAllHandler->onBody(nullptr);
}

void AsyncWebServer::_handleDisconnect(AsyncWebServerRequest* request) {
    _requests.erase(request);
    delete request;
}

void AsyncWebServer::_attachHandler(AsyncWebServerRequest* request) {
    for (auto& h : _handlers) {
        if (h.ptr->filter(request) && h.ptr->canHandle(request)) {
            request->setHandler(h.ptr);
            return;
        }
    }
    request->setHandler(_catchAllHandler.get());
}

void AsyncWebServer::_rewriteRequest(AsyncWebServerRequest* request) {
    for (auto& r : _rewrites) {
        if (r.ptr->match(request)) {
            request->_url = r.ptr->toUrl();
            request->_addGetParams(r.ptr->params());
        }
    }
}
