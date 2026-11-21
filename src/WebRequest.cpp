// AsyncWebServerRequest: incremental HTTP/1.x request parser, parameter and
// header access, authentication, and the send()/beginResponse() family.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>

#include "ESPAsyncWebServer.h"
#include "WebUtil.h"

using namespace AsyncEmu::web;

namespace {
const String kEmpty;
constexpr size_t kMaxLine = 16 * 1024;       // request line / header line
constexpr size_t kMaxFormBody = 1024 * 1024;  // buffered urlencoded bodies
constexpr size_t kMaxMultipartField = 64 * 1024;
}  // namespace

// ---------------------------------------------------------------------------
AsyncWebServerRequest::AsyncWebServerRequest(AsyncWebServer* server, AsyncClient* client) : _server(server), _client(client) {
    _server->_trackRequest(this);
    client->onError([](void* r, AsyncClient*, int8_t error) { static_cast<AsyncWebServerRequest*>(r)->_onError(error); }, this);
    client->onAck([](void* r, AsyncClient*, size_t len, uint32_t time) { static_cast<AsyncWebServerRequest*>(r)->_onAck(len, time); }, this);
    client->onDisconnect(
        [](void* r, AsyncClient* c) {
            static_cast<AsyncWebServerRequest*>(r)->_onDisconnect();  // deletes the request
            delete c;
        },
        this);
    client->onTimeout([](void* r, AsyncClient*, uint32_t time) { static_cast<AsyncWebServerRequest*>(r)->_onTimeout(time); }, this);
    client->onData([](void* r, AsyncClient*, void* buf, size_t len) { static_cast<AsyncWebServerRequest*>(r)->_onData(buf, len); }, this);
    client->onPoll([](void* r, AsyncClient*) { static_cast<AsyncWebServerRequest*>(r)->_onPoll(); }, this);
}

AsyncWebServerRequest::~AsyncWebServerRequest() {
    delete _response;
    _response = nullptr;
    if (_tempObject) std::free(_tempObject);
}

// ---------------------------------------------------------------------------
// Client events
// ---------------------------------------------------------------------------
void AsyncWebServerRequest::_onPoll() {
    if (_response && _client && _client->canSend() && !_response->_finished()) _response->_ack(this, 0, 0);
}

void AsyncWebServerRequest::_onAck(size_t len, uint32_t time) {
    if (_response && !_response->_finished()) _response->_ack(this, len, time);
}

void AsyncWebServerRequest::_onError(int8_t) {}

void AsyncWebServerRequest::_onTimeout(uint32_t) {
    if (_client) _client->close();
}

void AsyncWebServerRequest::_onDisconnect() {
    if (_onDisconnectfn) _onDisconnectfn();
    _server->_handleDisconnect(this);  // deletes this
}

void AsyncWebServerRequest::_finishResponse() {
    // Every response says "Connection: close"; the request is deleted when the client disconnects.
    if (_client) _client->close();
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
void AsyncWebServerRequest::_onData(void* buf, size_t len) {
    if (_response || _parseState == PARSE_REQ_FAIL || _parseState == PARSE_REQ_END) return;
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t i = 0;
    while (i < len && _parseState < PARSE_REQ_BODY) {
        char c = static_cast<char>(p[i++]);
        if (c == '\n') {
            _parseLine();
            _line.clear();
            if (_response || _parseState == PARSE_REQ_FAIL) return;
        } else {
            _line.push_back(c);
            if (_line.size() > kMaxLine) { _fail(431); return; }
        }
    }
    if (_parseState == PARSE_REQ_BODY && i < len) _feedBody(p + i, len - i);
}

void AsyncWebServerRequest::_fail(int code) {
    _parseState = PARSE_REQ_FAIL;
    if (!_response) send(code);
}

void AsyncWebServerRequest::_parseLine() {
    if (!_line.empty() && _line.back() == '\r') _line.pop_back();
    if (_parseState == PARSE_REQ_START) {
        if (_line.empty()) return;  // tolerate stray blank lines before the request line
        _parseRequestLine(_line);
    } else if (_parseState == PARSE_REQ_HEADERS) {
        if (_line.empty()) _headersComplete();
        else _parseHeaderLine(_line);
    }
}

void AsyncWebServerRequest::_parseRequestLine(const std::string& line) {
    size_t a = line.find(' ');
    size_t b = a == std::string::npos ? a : line.find(' ', a + 1);
    if (a == std::string::npos || b == std::string::npos) { _fail(400); return; }
    std::string method = line.substr(0, a);
    std::string target = line.substr(a + 1, b - a - 1);
    std::string ver = line.substr(b + 1);

    static const std::map<std::string, WebRequestMethod> kMethods = {
        {"GET", HTTP_GET}, {"POST", HTTP_POST}, {"DELETE", HTTP_DELETE}, {"PUT", HTTP_PUT},
        {"PATCH", HTTP_PATCH}, {"HEAD", HTTP_HEAD}, {"OPTIONS", HTTP_OPTIONS}};
    auto m = kMethods.find(method);
    if (m == kMethods.end()) { _fail(501); return; }
    _method = m->second;

    if (ver == "HTTP/1.1") _version = 1;
    else if (ver == "HTTP/1.0") _version = 0;
    else { _fail(505); return; }

    // absolute-form ("GET http://host/path HTTP/1.1")
    if (target.compare(0, 7, "http://") == 0 || target.compare(0, 8, "https://") == 0) {
        size_t s = target.find('/', target.find("//") + 2);
        target = s == std::string::npos ? "/" : target.substr(s);
    }
    size_t hash = target.find('#');
    if (hash != std::string::npos) target.erase(hash);
    size_t q = target.find('?');
    std::string path = q == std::string::npos ? target : target.substr(0, q);
    _url = AsyncEmu::web::urlDecode(path, false);
    if (_url.empty()) _url = "/";
    if (q != std::string::npos) _parseQuery(target.substr(q + 1), false);
    _parseState = PARSE_REQ_HEADERS;
}

void AsyncWebServerRequest::_parseHeaderLine(const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return;  // ignore malformed header lines
    std::string name = trim(line.substr(0, colon));
    std::string value = trim(line.substr(colon + 1));
    if (name.empty()) return;
    _headers.emplace_back(String(name), String(value));

    std::string lname = toLower(name);
    if (lname == "host") {
        _host = value;
    } else if (lname == "content-type") {
        size_t semi = value.find(';');
        _contentType = trim(value.substr(0, semi));
        if (toLower(_contentType).compare(0, 10, "multipart/") == 0) {
            size_t b = toLower(value).find("boundary=");
            if (b != std::string::npos) {
                std::string bd = value.substr(b + 9);
                size_t end = bd.find(';');
                if (end != std::string::npos) bd.erase(end);
                bd = trim(bd);
                if (bd.size() >= 2 && bd.front() == '"' && bd.back() == '"') bd = bd.substr(1, bd.size() - 2);
                _boundary = bd;
                _isMultipart = true;
            }
        }
    } else if (lname == "content-length") {
        _contentLength = static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
    } else if (lname == "expect") {
        _expectingContinue = iequals(value, "100-continue");
    } else if (lname == "authorization") {
        _authorization = value;
    } else if (lname == "upgrade") {
        if (iequals(value, "websocket")) _reqconntype = RCT_WS;
    } else if (lname == "accept") {
        if (toLower(value).compare(0, 17, "text/event-stream") == 0) _reqconntype = RCT_EVENT;
    }
}

void AsyncWebServerRequest::_headersComplete() {
    _server->_rewriteRequest(this);
    _server->_attachHandler(this);
    if (_contentLength == 0) {
        _parseState = PARSE_REQ_END;
        _handleRequest();
        return;
    }
    _parseState = PARSE_REQ_BODY;
    if (_expectingContinue && _client) _client->write("HTTP/1.1 100 Continue\r\n\r\n");
    if (_isMultipart) {
        _mp.boundary = "--" + std::string(_boundary);
        _mp.delim = "\r\n--" + std::string(_boundary);
    }
    _isPlainPost = !_isMultipart && toLower(_contentType).compare(0, 33, "application/x-www-form-urlencoded") == 0;
}

void AsyncWebServerRequest::_feedBody(const uint8_t* p, size_t n) {
    size_t remaining = _contentLength - _parsedLength;
    if (n > remaining) n = remaining;  // ignore anything beyond Content-Length

    if (_isMultipart) {
        _mpFeed(p, n);
    } else if (_isPlainPost) {
        if (_bodyBuf.size() + n > kMaxFormBody) { _fail(413); return; }
        _bodyBuf.append(reinterpret_cast<const char*>(p), n);
    } else if (_handler) {
        _handler->handleBody(this, const_cast<uint8_t*>(p), n, _parsedLength, _contentLength);
        if (_response) return;  // handler answered early (e.g. rejected the body)
    }
    _parsedLength += n;

    if (_parsedLength >= _contentLength) {
        if (_isPlainPost) _parseQuery(_bodyBuf, true);
        _parseState = PARSE_REQ_END;
        _handleRequest();
    }
}

void AsyncWebServerRequest::_handleRequest() {
    if (_handler) _handler->handleRequest(this);
    else send(501);
}

void AsyncWebServerRequest::_parseQuery(const std::string& q, bool post) {
    size_t i = 0;
    while (i <= q.size()) {
        size_t amp = q.find('&', i);
        if (amp == std::string::npos) amp = q.size();
        std::string pair = q.substr(i, amp - i);
        i = amp + 1;
        if (pair.empty()) continue;
        size_t eq = pair.find('=');
        String name = AsyncEmu::web::urlDecode(eq == std::string::npos ? pair : pair.substr(0, eq), true);
        String value = eq == std::string::npos ? String() : AsyncEmu::web::urlDecode(pair.substr(eq + 1), true);
        _params.emplace_back(name, value, post);
    }
}

void AsyncWebServerRequest::_addGetParams(const String& params) { _parseQuery(params, false); }

// ---------------------------------------------------------------------------
// multipart/form-data
// ---------------------------------------------------------------------------
bool AsyncWebServerRequest::_bodyHandlerActive() const { return _handler && !_handler->isRequestHandlerTrivial(); }

static std::string dispositionParam(const std::string& header, const std::string& key) {
    size_t i = 0;
    while (i < header.size()) {
        size_t semi = header.find(';', i);
        if (semi == std::string::npos) semi = header.size();
        std::string tok = trim(header.substr(i, semi - i));
        i = semi + 1;
        if (tok.compare(0, key.size() + 1, key + "=") == 0) {
            std::string v = trim(tok.substr(key.size() + 1));
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            return v;
        }
    }
    return std::string();
}

void AsyncWebServerRequest::_mpStartItem(const std::string& block) {
    _mp.name = String();
    _mp.filename = String();
    _mp.isFile = false;
    _mp.itemSize = 0;
    _mp.value.clear();
    size_t i = 0;
    while (i < block.size()) {
        size_t eol = block.find("\r\n", i);
        if (eol == std::string::npos) eol = block.size();
        std::string line = block.substr(i, eol - i);
        i = eol + 2;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (iequals(trim(line.substr(0, colon)), "Content-Disposition")) {
            std::string v = line.substr(colon + 1);
            _mp.name = dispositionParam(v, "name");
            std::string fn = dispositionParam(v, "filename");
            // A "filename" parameter - even an empty one - marks a file part.
            _mp.isFile = v.find("filename=") != std::string::npos;
            _mp.filename = fn;
        }
    }
}

void AsyncWebServerRequest::_mpEmit(const char* data, size_t len) {
    if (_mp.isFile) {
        if (_bodyHandlerActive() && len) {
            _handler->handleUpload(this, _mp.filename, _mp.itemSize, reinterpret_cast<uint8_t*>(const_cast<char*>(data)), len, false);
        }
        _mp.itemSize += len;
    } else if (_mp.value.size() < kMaxMultipartField) {
        _mp.value.append(data, len);
    }
}

void AsyncWebServerRequest::_mpFinishItem() {
    if (_mp.isFile) {
        if (_bodyHandlerActive()) {
            static uint8_t none[1] = {0};
            _handler->handleUpload(this, _mp.filename, _mp.itemSize, none, 0, true);
        }
        _params.emplace_back(_mp.name, _mp.filename, true, true, _mp.itemSize);
    } else {
        _params.emplace_back(_mp.name, String(_mp.value), true);
    }
}

void AsyncWebServerRequest::_mpFeed(const uint8_t* p, size_t n) {
    _mp.buf.append(reinterpret_cast<const char*>(p), n);
    for (;;) {
        if (_response) return;
        switch (_mp.st) {
            case Multipart::PREAMBLE: {
                size_t pos = _mp.buf.find(_mp.boundary);
                if (pos == std::string::npos) {
                    // keep a possible partial boundary at the end
                    if (_mp.buf.size() >= _mp.boundary.size()) _mp.buf.erase(0, _mp.buf.size() - (_mp.boundary.size() - 1));
                    return;
                }
                _mp.buf.erase(0, pos + _mp.boundary.size());
                _mp.st = Multipart::AFTER_BOUNDARY;
                continue;
            }
            case Multipart::AFTER_BOUNDARY: {
                if (_mp.buf.size() < 2) return;
                if (_mp.buf.compare(0, 2, "--") == 0) { _mp.st = Multipart::DONE; _mp.buf.clear(); return; }
                size_t eol = _mp.buf.find("\r\n");
                if (eol == std::string::npos) return;
                _mp.buf.erase(0, eol + 2);
                _mp.st = Multipart::HEADERS;
                continue;
            }
            case Multipart::HEADERS: {
                size_t end = _mp.buf.find("\r\n\r\n");
                if (end == std::string::npos) {
                    if (_mp.buf.size() > kMaxLine) _fail(431);
                    // part with no headers at all: "\r\n" directly
                    return;
                }
                std::string block = _mp.buf.substr(0, end);
                _mp.buf.erase(0, end + 4);
                _mpStartItem(block);
                _mp.st = Multipart::BODY;
                continue;
            }
            case Multipart::BODY: {
                size_t pos = _mp.buf.find(_mp.delim);
                if (pos != std::string::npos) {
                    _mpEmit(_mp.buf.data(), pos);
                    _mpFinishItem();
                    _mp.buf.erase(0, pos + _mp.delim.size());
                    _mp.st = Multipart::AFTER_BOUNDARY;
                    continue;
                }
                if (_mp.buf.size() >= _mp.delim.size()) {
                    size_t emit = _mp.buf.size() - (_mp.delim.size() - 1);  // the tail may start a delimiter
                    _mpEmit(_mp.buf.data(), emit);
                    _mp.buf.erase(0, emit);
                }
                return;
            }
            case Multipart::DONE:
                _mp.buf.clear();
                return;
        }
    }
}

// ---------------------------------------------------------------------------
// Params and headers
// ---------------------------------------------------------------------------
void AsyncWebServerRequest::_addParam(AsyncWebParameter* p) {
    if (!p) return;
    _params.push_back(*p);
    delete p;
}

void AsyncWebServerRequest::_addPathParam(const char* param) { _pathParams.emplace_back(param ? param : ""); }
const String& AsyncWebServerRequest::pathArg(size_t i) const { return i < _pathParams.size() ? _pathParams[i] : kEmpty; }

bool AsyncWebServerRequest::hasParam(const String& name, bool post, bool file) const { return getParam(name, post, file) != nullptr; }

AsyncWebParameter* AsyncWebServerRequest::getParam(const String& name, bool post, bool file) const {
    for (auto& p : _params)
        if (p.name() == name && p.isPost() == post && p.isFile() == file) return const_cast<AsyncWebParameter*>(&p);
    return nullptr;
}

AsyncWebParameter* AsyncWebServerRequest::getParam(size_t num) const {
    return num < _params.size() ? const_cast<AsyncWebParameter*>(&_params[num]) : nullptr;
}

const String& AsyncWebServerRequest::arg(const String& name) const {
    for (auto& p : _params)
        if (!p.isFile() && p.name() == name) return p.value();
    return kEmpty;
}

const String& AsyncWebServerRequest::arg(size_t i) const { return i < _params.size() ? _params[i].value() : kEmpty; }
const String& AsyncWebServerRequest::argName(size_t i) const { return i < _params.size() ? _params[i].name() : kEmpty; }

bool AsyncWebServerRequest::hasArg(const char* name) const {
    for (auto& p : _params)
        if (!p.isFile() && p.name() == name) return true;
    return false;
}

bool AsyncWebServerRequest::hasHeader(const String& name) const { return getHeader(name) != nullptr; }

AsyncWebHeader* AsyncWebServerRequest::getHeader(const String& name) const {
    for (auto& h : _headers)
        if (h.name().equalsIgnoreCase(name)) return const_cast<AsyncWebHeader*>(&h);
    return nullptr;
}

AsyncWebHeader* AsyncWebServerRequest::getHeader(size_t num) const {
    return num < _headers.size() ? const_cast<AsyncWebHeader*>(&_headers[num]) : nullptr;
}

const String& AsyncWebServerRequest::header(const char* name) const {
    AsyncWebHeader* h = getHeader(String(name));
    return h ? h->value() : kEmpty;
}
const String& AsyncWebServerRequest::header(size_t num) const { return num < _headers.size() ? _headers[num].value() : kEmpty; }
const String& AsyncWebServerRequest::headerName(size_t num) const { return num < _headers.size() ? _headers[num].name() : kEmpty; }

String AsyncWebServerRequest::urlDecode(const String& text) const { return AsyncEmu::web::urlDecode(text, true); }

const char* AsyncWebServerRequest::methodToString() const {
    switch (_method) {
        case HTTP_GET: return "GET";
        case HTTP_POST: return "POST";
        case HTTP_DELETE: return "DELETE";
        case HTTP_PUT: return "PUT";
        case HTTP_PATCH: return "PATCH";
        case HTTP_HEAD: return "HEAD";
        case HTTP_OPTIONS: return "OPTIONS";
        default: return "UNKNOWN";
    }
}

const char* AsyncWebServerRequest::requestedConnTypeToString() const {
    switch (_reqconntype) {
        case RCT_NOT_USED: return "RCT_NOT_USED";
        case RCT_DEFAULT: return "RCT_DEFAULT";
        case RCT_HTTP: return "RCT_HTTP";
        case RCT_WS: return "RCT_WS";
        case RCT_EVENT: return "RCT_EVENT";
        default: return "ERROR";
    }
}

bool AsyncWebServerRequest::isExpectedRequestedConnType(RequestedConnectionType a, RequestedConnectionType b, RequestedConnectionType c) const {
    // RCT_DEFAULT stands for "a plain HTTP request"
    auto norm = [](RequestedConnectionType t) { return t == RCT_DEFAULT ? RCT_HTTP : t; };
    RequestedConnectionType cur = norm(_reqconntype);
    return (a != RCT_NOT_USED && norm(a) == cur) || (b != RCT_NOT_USED && norm(b) == cur) || (c != RCT_NOT_USED && norm(c) == cur);
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------
static std::map<std::string, std::string> parseDigest(const std::string& s) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == ',')) ++i;
        size_t eq = s.find('=', i);
        if (eq == std::string::npos) break;
        std::string key = toLower(trim(s.substr(i, eq - i)));
        i = eq + 1;
        std::string val;
        if (i < s.size() && s[i] == '"') {
            size_t end = s.find('"', i + 1);
            if (end == std::string::npos) end = s.size();
            val = s.substr(i + 1, end - i - 1);
            i = end + 1;
        } else {
            size_t end = s.find(',', i);
            if (end == std::string::npos) end = s.size();
            val = trim(s.substr(i, end - i));
            i = end;
        }
        out[key] = val;
    }
    return out;
}

bool AsyncWebServerRequest::authenticate(const char* hash) {
    if (!hash || _authorization.compare(0, 6, "Basic ") != 0) return false;
    return trim(_authorization.substr(6)) == hash;
}

bool AsyncWebServerRequest::authenticate(const char* username, const char* password, const char* realm, bool passwordIsHash) {
    if (_authorization.empty() || !username || !password) return false;

    if (_authorization.compare(0, 6, "Basic ") == 0) {
        if (passwordIsHash) return authenticate(password);
        return authenticate(base64Encode(std::string(username) + ":" + password).c_str());
    }

    if (_authorization.compare(0, 7, "Digest ") == 0) {
        auto f = parseDigest(_authorization.substr(7));
        if (f["username"] != username) return false;
        std::string expectedRealm = realm ? realm : "";
        if (f["realm"] != expectedRealm) return false;
        std::string ha1 = passwordIsHash ? std::string(password) : md5Hex(std::string(username) + ":" + f["realm"] + ":" + password);
        std::string ha2 = md5Hex(std::string(methodToString()) + ":" + f["uri"]);
        std::string expected = f.count("qop") && !f["qop"].empty()
                                   ? md5Hex(ha1 + ":" + f["nonce"] + ":" + f["nc"] + ":" + f["cnonce"] + ":" + f["qop"] + ":" + ha2)
                                   : md5Hex(ha1 + ":" + f["nonce"] + ":" + ha2);
        return toLower(f["response"]) == expected;
    }
    return false;
}

void AsyncWebServerRequest::requestAuthentication(const char* realm, bool isDigest) {
    AsyncWebServerResponse* r = beginResponse(401);
    if (!isDigest) {
        r->addHeader("WWW-Authenticate", String("Basic realm=\"") + (realm ? realm : "Login Required") + "\"");
    } else {
        r->addHeader("WWW-Authenticate", String("Digest realm=\"") + (realm ? realm : "") + "\", qop=\"auth\", nonce=\"" +
                                             randomHex32() + "\", opaque=\"" + randomHex32() + "\"");
    }
    send(r);
}

// ---------------------------------------------------------------------------
// Responding
// ---------------------------------------------------------------------------
void AsyncWebServerRequest::send(AsyncWebServerResponse* response) {
    if (_response) { delete response; return; }  // already answered
    if (!response) {
        if (_client) _client->close(true);
        return;
    }
    if (!response->_sourceValid()) {
        delete response;
        send(500);
        return;
    }
    _response = response;
    if (_client) _client->setRxTimeout(0);  // the request is complete: no more idle timeout
    _response->_respond(this);
}

void AsyncWebServerRequest::send(int code, const String& contentType, const String& content) { send(beginResponse(code, contentType, content)); }

void AsyncWebServerRequest::send(FS& fs, const String& path, const String& contentType, bool download, AwsTemplateProcessor callback) {
    if (fs.exists(path) || (!download && fs.exists(path + ".gz"))) send(beginResponse(fs, path, contentType, download, std::move(callback)));
    else send(404);
}

void AsyncWebServerRequest::send(File content, const String& path, const String& contentType, bool download, AwsTemplateProcessor callback) {
    if (content) send(beginResponse(std::move(content), path, contentType, download, std::move(callback)));
    else send(404);
}

void AsyncWebServerRequest::send(const String& contentType, size_t len, AwsResponseFiller callback, AwsTemplateProcessor templateCallback) {
    send(beginResponse(contentType, len, std::move(callback), std::move(templateCallback)));
}

void AsyncWebServerRequest::sendChunked(const String& contentType, AwsResponseFiller callback, AwsTemplateProcessor templateCallback) {
    send(beginChunkedResponse(contentType, std::move(callback), std::move(templateCallback)));
}

void AsyncWebServerRequest::send_P(int code, const String& contentType, const uint8_t* content, size_t len, AwsTemplateProcessor callback) {
    send(beginResponse_P(code, contentType, content, len, std::move(callback)));
}

void AsyncWebServerRequest::send_P(int code, const String& contentType, const char* content, AwsTemplateProcessor callback) {
    send(beginResponse_P(code, contentType, content, std::move(callback)));
}

void AsyncWebServerRequest::redirect(const String& url) {
    AsyncWebServerResponse* response = beginResponse(302);
    response->addHeader("Location", url);
    send(response);
}

AsyncWebServerResponse* AsyncWebServerRequest::beginResponse(int code, const String& contentType, const String& content) {
    return new AsyncBasicResponse(code, contentType, content);
}

AsyncWebServerResponse* AsyncWebServerRequest::beginResponse(FS& fs, const String& path, const String& contentType, bool download,
                                                             AwsTemplateProcessor callback) {
    return new AsyncFileResponse(fs, path, contentType, download, std::move(callback));
}

AsyncWebServerResponse* AsyncWebServerRequest::beginResponse(File content, const String& path, const String& contentType, bool download,
                                                             AwsTemplateProcessor callback) {
    return new AsyncFileResponse(std::move(content), path, contentType, download, std::move(callback));
}

AsyncWebServerResponse* AsyncWebServerRequest::beginResponse(const String& contentType, size_t len, AwsResponseFiller callback,
                                                             AwsTemplateProcessor templateCallback) {
    return new AsyncCallbackResponse(contentType, len, std::move(callback), std::move(templateCallback));
}

AsyncWebServerResponse* AsyncWebServerRequest::beginChunkedResponse(const String& contentType, AwsResponseFiller callback,
                                                                    AwsTemplateProcessor templateCallback) {
    return new AsyncChunkedResponse(contentType, std::move(callback), std::move(templateCallback));
}

AsyncResponseStream* AsyncWebServerRequest::beginResponseStream(const String& contentType, size_t bufferSize) {
    return new AsyncResponseStream(contentType, bufferSize);
}

AsyncWebServerResponse* AsyncWebServerRequest::beginResponse_P(int code, const String& contentType, const uint8_t* content, size_t len,
                                                               AwsTemplateProcessor callback) {
    return new AsyncProgmemResponse(code, contentType, content, len, std::move(callback));
}

AsyncWebServerResponse* AsyncWebServerRequest::beginResponse_P(int code, const String& contentType, const char* content,
                                                               AwsTemplateProcessor callback) {
    return new AsyncProgmemResponse(code, contentType, reinterpret_cast<const uint8_t*>(content), std::strlen(content), std::move(callback));
}
