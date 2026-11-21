// Response classes and the send engine.
//
// The engine (AsyncAbstractResponse::_pump) writes the head and then the body
// in pieces that fit the AsyncClient's send buffer (space()), and is re-run
// from the client's onAck - the same flow control the real library uses.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "ESPAsyncWebServer.h"
#include "WebUtil.h"

using namespace AsyncEmu::web;

// ---------------------------------------------------------------------------
AsyncWebHeader::AsyncWebHeader(const String& data) {
    int idx = data.indexOf(':');
    if (idx < 0) return;
    _name = data.substring(0, static_cast<size_t>(idx));
    _value = data.substring(static_cast<size_t>(idx) + 1);
    _name.trim();
    _value.trim();
}

// ---------------------------------------------------------------------------
// AsyncWebServerResponse
// ---------------------------------------------------------------------------
AsyncWebServerResponse::AsyncWebServerResponse()
    : _code(0), _contentLength(0), _sendContentLength(true), _chunked(false), _state(RESPONSE_SETUP) {}

void AsyncWebServerResponse::setCode(int code) { if (_state == RESPONSE_SETUP) _code = code; }
void AsyncWebServerResponse::setContentLength(size_t len) { if (_state == RESPONSE_SETUP && _sendContentLength) _contentLength = len; }
void AsyncWebServerResponse::setContentType(const String& type) { if (_state == RESPONSE_SETUP) _contentType = type; }
void AsyncWebServerResponse::addHeader(const String& name, const String& value) { _headers.emplace_back(name, value); }

const char* AsyncWebServerResponse::responseCodeToString(int code) { return statusText(code); }

String AsyncWebServerResponse::_assembleHead(uint8_t version) {
    if (version) {
        addHeader("Accept-Ranges", "none");
        if (_chunked) addHeader("Transfer-Encoding", "chunked");
    }
    String out;
    char buf[300];
    std::snprintf(buf, sizeof buf, "HTTP/1.%d %d %s\r\n", version, _code, statusText(_code));
    out.concat(buf);
    if (_sendContentLength) {
        std::snprintf(buf, sizeof buf, "Content-Length: %u\r\n", static_cast<unsigned>(_contentLength));
        out.concat(buf);
    }
    if (_contentType.length()) {
        out.concat("Content-Type: ");
        out.concat(_contentType);
        out.concat("\r\n");
    }
    for (auto& h : _headers) out.concat(h.toString());
    for (auto& h : DefaultHeaders::Instance().headers()) {
        bool present = false;
        for (auto& mine : _headers) present = present || mine.name().equalsIgnoreCase(h.name());
        if (!present) out.concat(h.toString());
    }
    out.concat("\r\n");
    return out;
}

// ---------------------------------------------------------------------------
// AsyncAbstractResponse
// ---------------------------------------------------------------------------
AsyncAbstractResponse::AsyncAbstractResponse(AwsTemplateProcessor callback) : _callback(std::move(callback)) {
    if (_callback) _sendContentLength = false;  // size changes while processing
}

void AsyncAbstractResponse::_respond(AsyncWebServerRequest* request) {
    bool hasConnection = false;
    for (auto& h : _headers) hasConnection = hasConnection || h.name().equalsIgnoreCase("Connection");
    if (!hasConnection) addHeader("Connection", "close");

    _isHead = request->method() == HTTP_HEAD;
    if (_code < 200 || _code == 204 || _code == 304) {
        _bodyAllowed = false;
        _sendContentLength = false;
    }
    // Unknown length: chunked on HTTP/1.1, otherwise the body ends with the connection.
    _chunked = _bodyAllowed && !_sendContentLength && request->version() > 0 && !_isHead;
    _head = _assembleHead(request->version());
    _state = RESPONSE_HEADERS;
    _pump(request);
}

size_t AsyncAbstractResponse::_ack(AsyncWebServerRequest* request, size_t len, uint32_t) {
    _ackedLength += len;
    _pump(request);
    return 0;
}

void AsyncAbstractResponse::_pump(AsyncWebServerRequest* request) {
    AsyncClient* c = request->client();
    if (!c) return;
    while (_state != RESPONSE_END && _state != RESPONSE_FAILED) {
        if (_state == RESPONSE_HEADERS) {
            if (!c->canSend()) return;
            size_t n = c->write(_head.c_str() + _headWritten, _head.size() - _headWritten);
            _headWritten += n;
            _writtenLength += n;
            if (_headWritten < _head.size()) return;  // wait for acks to free buffer space
            if (_isHead || !_bodyAllowed || (!_chunked && _sendContentLength && _contentLength == 0))
                _state = RESPONSE_WAIT_ACK;
            else
                _state = RESPONSE_CONTENT;
            continue;
        }

        if (_state == RESPONSE_CONTENT) {
            size_t space = c->space();
            if (_chunked) {
                if (space <= 16) return;  // not even room for framing
                size_t maxData = std::min<size_t>(space - 16, 8192);
                std::vector<uint8_t> tmp(maxData);
                size_t got = _callback ? _fillProcessed(tmp.data(), maxData) : _fillBuffer(tmp.data(), maxData);
                if (got == RESPONSE_TRY_AGAIN) return;  // retried from onAck / onPoll
                if (got == 0) {
                    c->write("0\r\n\r\n", 5);
                    _writtenLength += 5;
                    _state = RESPONSE_WAIT_ACK;
                    continue;
                }
                char hdr[24];
                int hn = std::snprintf(hdr, sizeof hdr, "%zx\r\n", got);
                std::string out(hdr, static_cast<size_t>(hn));
                out.append(reinterpret_cast<char*>(tmp.data()), got);
                out += "\r\n";
                c->write(out.data(), out.size());
                _writtenLength += out.size();
                _sentLength += got;
                continue;
            }

            if (space == 0) return;
            size_t remaining = _sendContentLength ? _contentLength - _sentLength : static_cast<size_t>(-1);
            size_t want = std::min<size_t>({space, remaining, 8192});
            std::vector<uint8_t> tmp(want);
            size_t got = _callback ? _fillProcessed(tmp.data(), want) : _fillBuffer(tmp.data(), want);
            if (got == RESPONSE_TRY_AGAIN) return;
            if (got == 0) {
                if (_sendContentLength && _sentLength < _contentLength) {
                    // The source ended early: the client can't be told, so drop the connection.
                    _state = RESPONSE_FAILED;
                    c->close();
                    return;
                }
                _state = RESPONSE_WAIT_ACK;
                continue;
            }
            c->write(reinterpret_cast<char*>(tmp.data()), got);
            _sentLength += got;
            _writtenLength += got;
            if (_sendContentLength && _sentLength >= _contentLength) _state = RESPONSE_WAIT_ACK;
            continue;
        }

        if (_state == RESPONSE_WAIT_ACK) {
            if (_ackedLength >= _writtenLength) {
                _state = RESPONSE_END;
                request->_finishResponse();  // may lead to the request being destroyed: touch nothing afterwards
            }
            return;
        }
        return;
    }
}

// Template processing: replaces %NAME% placeholders. "%%" yields a literal "%",
// and a '%' that doesn't start a well-formed placeholder is passed through.
size_t AsyncAbstractResponse::_fillProcessed(uint8_t* buf, size_t maxLen) {
    auto processRaw = [this]() -> bool {
        if (_raw.empty()) return false;
        size_t p = _raw.find('%');
        if (p == std::string::npos) { _cache.append(_raw); _raw.clear(); return true; }
        if (p > 0) { _cache.append(_raw, 0, p); _raw.erase(0, p); return true; }
        size_t q = _raw.find('%', 1);
        if (q == std::string::npos) {
            if (_raw.size() < 64 && !_srcDone) return false;  // the closing '%' may still arrive
            _cache.push_back('%');
            _raw.erase(0, 1);
            return true;
        }
        std::string name = _raw.substr(1, q - 1);
        if (name.empty()) { _cache.push_back('%'); _raw.erase(0, 2); return true; }
        bool valid = name.size() < 64;
        for (char ch : name) valid = valid && !std::isspace(static_cast<unsigned char>(ch));
        if (!valid) { _cache.push_back('%'); _raw.erase(0, 1); return true; }
        _cache.append(_callback(String(name)));
        _raw.erase(0, q + 1);
        return true;
    };

    size_t out = 0;
    while (out < maxLen) {
        if (!_cache.empty()) {
            size_t n = std::min(maxLen - out, _cache.size());
            std::memcpy(buf + out, _cache.data(), n);
            _cache.erase(0, n);
            out += n;
            continue;
        }
        if (processRaw()) continue;
        if (_srcDone) break;
        uint8_t tmp[1024];
        size_t r = _fillBuffer(tmp, sizeof tmp);
        if (r == RESPONSE_TRY_AGAIN) {
            if (out) break;
            return RESPONSE_TRY_AGAIN;
        }
        if (r == 0) { _srcDone = true; continue; }
        _raw.append(reinterpret_cast<char*>(tmp), r);
        _filledLength += r;
    }
    return out;
}

// ---------------------------------------------------------------------------
// AsyncBasicResponse
// ---------------------------------------------------------------------------
AsyncBasicResponse::AsyncBasicResponse(int code, const String& contentType, const String& content) {
    _code = code;
    _content = content;
    _contentType = contentType;
    if (_content.length()) {
        _contentLength = _content.length();
        if (!_contentType.length()) _contentType = "text/plain";
    }
}

size_t AsyncBasicResponse::_fillBuffer(uint8_t* buf, size_t maxLen) {
    size_t n = std::min(maxLen, _content.size() - _pos);
    std::memcpy(buf, _content.data() + _pos, n);
    _pos += n;
    return n;
}

// ---------------------------------------------------------------------------
// AsyncCallbackResponse / AsyncChunkedResponse
// ---------------------------------------------------------------------------
AsyncCallbackResponse::AsyncCallbackResponse(const String& contentType, size_t len, AwsResponseFiller callback,
                                             AwsTemplateProcessor templateCallback)
    : AsyncAbstractResponse(std::move(templateCallback)), _content(std::move(callback)) {
    _code = 200;
    _contentType = contentType;
    _contentLength = len;
    if (!len) _sendContentLength = false;
}

size_t AsyncCallbackResponse::_fillBuffer(uint8_t* buf, size_t maxLen) {
    size_t ret = _content(buf, maxLen, _filled);
    if (ret != RESPONSE_TRY_AGAIN) _filled += ret;
    return ret;
}

AsyncChunkedResponse::AsyncChunkedResponse(const String& contentType, AwsResponseFiller callback, AwsTemplateProcessor templateCallback)
    : AsyncAbstractResponse(std::move(templateCallback)), _content(std::move(callback)) {
    _code = 200;
    _contentType = contentType;
    _sendContentLength = false;
}

size_t AsyncChunkedResponse::_fillBuffer(uint8_t* buf, size_t maxLen) {
    size_t ret = _content(buf, maxLen, _filled);
    if (ret != RESPONSE_TRY_AGAIN) _filled += ret;
    return ret;
}

// ---------------------------------------------------------------------------
// AsyncResponseStream
// ---------------------------------------------------------------------------
AsyncResponseStream::AsyncResponseStream(const String& contentType, size_t bufferSize) {
    _code = 200;
    _contentType = contentType;
    _sendContentLength = false;
    _buf.reserve(bufferSize);
}

size_t AsyncResponseStream::write(const uint8_t* data, size_t len) {
    if (_finished()) return 0;
    _buf.append(reinterpret_cast<const char*>(data), len);
    return len;
}

size_t AsyncResponseStream::printf(const char* fmt, ...) {
    char stack[256];
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(stack, sizeof stack, fmt, ap);
    va_end(ap);
    size_t written = 0;
    if (n >= 0 && static_cast<size_t>(n) < sizeof stack) {
        written = write(reinterpret_cast<const uint8_t*>(stack), static_cast<size_t>(n));
    } else if (n > 0) {
        std::vector<char> big(static_cast<size_t>(n) + 1);
        std::vsnprintf(big.data(), big.size(), fmt, ap2);
        written = write(reinterpret_cast<const uint8_t*>(big.data()), static_cast<size_t>(n));
    }
    va_end(ap2);
    return written;
}

size_t AsyncResponseStream::_fillBuffer(uint8_t* buf, size_t maxLen) {
    size_t n = std::min(maxLen, _buf.size());
    std::memcpy(buf, _buf.data(), n);
    _buf.erase(0, n);
    return n;
}

// ---------------------------------------------------------------------------
// AsyncFileResponse
// ---------------------------------------------------------------------------
AsyncFileResponse::AsyncFileResponse(FS& fs, const String& path, const String& contentType, bool download, AwsTemplateProcessor callback)
    : AsyncAbstractResponse(std::move(callback)) {
    String p = path;
    if (!download && !fs.exists(p) && fs.exists(p + ".gz")) p += ".gz";
    _content = fs.open(p, "r");
    _init(p, contentType, download);
}

AsyncFileResponse::AsyncFileResponse(File content, const String& path, const String& contentType, bool download, AwsTemplateProcessor callback)
    : AsyncAbstractResponse(std::move(callback)), _content(std::move(content)) {
    _init(path, contentType, download);
}

void AsyncFileResponse::_init(const String& path, const String& contentType, bool download) {
    if (_content && _content.isDirectory()) _content.close();
    _code = 200;
    bool gz = path.endsWith(".gz");
    String base = gz ? path.substring(0, path.length() - 3) : path;
    String filename = base.substring(static_cast<size_t>(base.lastIndexOf('/') + 1));
    if (download) {
        _contentType = "application/octet-stream";
        addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
    } else {
        _contentType = contentType.length() ? contentType : mimeTypeForPath(base);
        addHeader("Content-Disposition", "inline");
    }
    if (gz) addHeader("Content-Encoding", "gzip");
    if (_content) _contentLength = _content.size();
}

size_t AsyncFileResponse::_fillBuffer(uint8_t* buf, size_t maxLen) { return _content.read(buf, maxLen); }

// ---------------------------------------------------------------------------
// AsyncProgmemResponse
// ---------------------------------------------------------------------------
AsyncProgmemResponse::AsyncProgmemResponse(int code, const String& contentType, const uint8_t* content, size_t len,
                                           AwsTemplateProcessor callback)
    : AsyncAbstractResponse(std::move(callback)), _data(content) {
    _code = code;
    _contentType = contentType;
    _contentLength = len;
}

size_t AsyncProgmemResponse::_fillBuffer(uint8_t* buf, size_t maxLen) {
    size_t n = std::min(maxLen, _contentLength - _pos);
    std::memcpy(buf, _data + _pos, n);
    _pos += n;
    return n;
}
