// JSON for the web server, API-compatible with ESPAsyncWebServer's AsyncJson.h.
//
//   // reply with JSON
//   AsyncJsonResponse* response = new AsyncJsonResponse();
//   response->getRoot()["uptime"] = millis();
//   response->setLength();                 // required: measures the document
//   request->send(response);
//
//   // receive JSON (POST/PUT/PATCH with Content-Type: application/json)
//   server.addHandler(new AsyncCallbackJsonWebHandler("/api/set", [](AsyncWebServerRequest* r, JsonVariant& json) {
//       r->send(200, "application/json", "{\"got\":" + String(json["x"].as<int>()) + "}");
//   }));
//
// Needs ArduinoJson (v6 or v7), which CMake fetches when ESPASYNCEMU_ENABLE_JSON is ON.
// Include "Arduino.h" before ArduinoJson.h elsewhere in your code if you want ArduinoJson's
// String/Print/Stream support for the emulated Arduino types.
#pragma once

#if !__has_include(<ArduinoJson.h>)
#error "AsyncJson.h needs ArduinoJson: build with -DESPASYNCEMU_ENABLE_JSON=ON (the default) or add it to the include path"
#endif

#include <ArduinoJson.h>

#include "Arduino.h"
#include "ESPAsyncWebServer.h"

#ifndef DYNAMIC_JSON_DOCUMENT_SIZE
#define DYNAMIC_JSON_DOCUMENT_SIZE 1024  // only used with ArduinoJson v6
#endif

constexpr const char* JSON_MIMETYPE = "application/json";
constexpr const char* MSGPACK_MIMETYPE = "application/x-msgpack";

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------
// Fill getRoot(), call setLength(), then request->send(response). A response whose
// setLength() was never called (or whose document is empty) is sent as a 500, as
// in the real library.
class AsyncJsonResponse : public AsyncAbstractResponse {
public:
    AsyncJsonResponse(bool isArray = false, size_t maxJsonBufferSize = DYNAMIC_JSON_DOCUMENT_SIZE)
#if ARDUINOJSON_VERSION_MAJOR >= 7
        : _isValid(false) {
        (void)maxJsonBufferSize;
        if (isArray) _root = _jsonBuffer.to<JsonArray>();
        else _root = _jsonBuffer.to<JsonObject>();
#else
        : _jsonBuffer(maxJsonBufferSize), _isValid(false) {
        if (isArray) _root = _jsonBuffer.createNestedArray();
        else _root = _jsonBuffer.createNestedObject();
#endif
        _code = 200;
        _contentType = JSON_MIMETYPE;
    }
    virtual ~AsyncJsonResponse() {}

    JsonVariant& getRoot() { return _root; }
    bool _sourceValid() const override { return _isValid; }

    size_t setLength() {
        _body.clear();
        serialize(_body);
        _contentLength = _body.size();
        if (_contentLength) _isValid = true;
        return _contentLength;
    }

    size_t getSize() { return _jsonBuffer.size(); }

    size_t _fillBuffer(uint8_t* data, size_t len) override {
        size_t n = std::min(len, _body.size() - _pos);
        std::memcpy(data, _body.data() + _pos, n);
        _pos += n;
        return n;
    }

protected:
    virtual void serialize(std::string& out) { serializeJson(_root, out); }

#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument _jsonBuffer;
#else
    DynamicJsonDocument _jsonBuffer;
#endif
    JsonVariant _root;
    bool _isValid;
    std::string _body;
    size_t _pos = 0;
};

// Indented output (handy when you read it in a browser or curl).
class PrettyAsyncJsonResponse : public AsyncJsonResponse {
public:
    PrettyAsyncJsonResponse(bool isArray = false, size_t maxJsonBufferSize = DYNAMIC_JSON_DOCUMENT_SIZE)
        : AsyncJsonResponse(isArray, maxJsonBufferSize) {}

protected:
    void serialize(std::string& out) override { serializeJsonPretty(_root, out); }
};

// Same idea for MessagePack (Content-Type: application/x-msgpack).
class AsyncMessagePackResponse : public AsyncJsonResponse {
public:
    AsyncMessagePackResponse(bool isArray = false, size_t maxJsonBufferSize = DYNAMIC_JSON_DOCUMENT_SIZE)
        : AsyncJsonResponse(isArray, maxJsonBufferSize) {
        _contentType = MSGPACK_MIMETYPE;
    }

protected:
    void serialize(std::string& out) override { serializeMsgPack(_root, out); }
};

// ---------------------------------------------------------------------------
// Request handler
// ---------------------------------------------------------------------------
typedef std::function<void(AsyncWebServerRequest* request, JsonVariant& json)> ArJsonRequestHandlerFunction;

// Collects a JSON request body, parses it and calls onRequest. Replies 400 for malformed
// or empty bodies and 413 when the body exceeds setMaxContentLength() (default 16 KB).
// Handles POST, PUT and PATCH by default, and only when the request's Content-Type is
// application/json. Add it with server.addHandler(new AsyncCallbackJsonWebHandler(...)); like
// every addHandler() object it is not owned by the server.
class AsyncCallbackJsonWebHandler : public AsyncWebHandler {
public:
    AsyncCallbackJsonWebHandler(const String& uri, ArJsonRequestHandlerFunction onRequest = nullptr,
                                size_t maxJsonBufferSize = DYNAMIC_JSON_DOCUMENT_SIZE)
        : _uri(uri), _method(HTTP_POST | HTTP_PUT | HTTP_PATCH), _onRequest(std::move(onRequest)), _maxJsonBufferSize(maxJsonBufferSize) {}

    void setMethod(WebRequestMethodComposite method) { _method = method; }
    void setMaxContentLength(size_t maxContentLength) { _maxContentLength = maxContentLength; }
    void onRequest(ArJsonRequestHandlerFunction fn) { _onRequest = std::move(fn); }

    bool canHandle(AsyncWebServerRequest* request) override {
        if (!_onRequest) return false;
        WebRequestMethodComposite m = request->method();
        if (!(_method & m)) return false;
        if (_uri.length() && (_uri != request->url() && !request->url().startsWith(_uri + "/"))) return false;
        if (m != HTTP_GET && !request->contentType().equalsIgnoreCase(JSON_MIMETYPE)) return false;
        request->addInterestingHeader("ANY");
        return true;
    }

    void handleRequest(AsyncWebServerRequest* request) override {
        if (!_onRequest) return request->send(500);
        size_t total = request->contentLength();
        if (total > _maxContentLength) return request->send(413);
        if (!total || !request->_tempObject) return request->send(400);

#if ARDUINOJSON_VERSION_MAJOR >= 7
        JsonDocument doc;
#else
        // v6 documents have a fixed capacity: use the configured size, or 8x the body if that is larger (v6 slots are 24 bytes on 64-bit hosts)
        DynamicJsonDocument doc(std::max(_maxJsonBufferSize, total * 8));
#endif
        DeserializationError error = deserializeJson(doc, static_cast<const char*>(request->_tempObject), total);
        if (error) return request->send(400);
        JsonVariant json = doc.as<JsonVariant>();
        _onRequest(request, json);
    }

    void handleBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) override {
        if (!_onRequest) return;
        if (total > _maxContentLength) {  // refuse early: don't buffer a body we will reject
            if (index == 0) request->send(413);
            return;
        }
        if (total > 0 && request->_tempObject == nullptr) {
            request->_tempObject = std::malloc(total + 1);  // freed with the request
            if (request->_tempObject) static_cast<char*>(request->_tempObject)[total] = 0;
        }
        if (request->_tempObject) std::memcpy(static_cast<uint8_t*>(request->_tempObject) + index, data, len);
    }

    bool isRequestHandlerTrivial() override { return !_onRequest; }

private:
    String _uri;
    WebRequestMethodComposite _method;
    ArJsonRequestHandlerFunction _onRequest;
    size_t _maxContentLength = 16384;
    size_t _maxJsonBufferSize;
};
