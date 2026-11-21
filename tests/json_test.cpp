// AsyncJson: JSON/MessagePack responses, the JSON request handler, and the
// String / Print / Stream interplay with ArduinoJson.
#include <filesystem>
#include <fstream>

#include "AsyncJson.h"
#include "http_test_util.h"

namespace stdfs = std::filesystem;

// ArduinoJson v7 documents grow on demand; v6 ones have a fixed capacity that you choose.
#if ARDUINOJSON_VERSION_MAJOR >= 7
#define MAKE_DOC(name) JsonDocument name
#define MAKE_BIG_DOC(name) JsonDocument name
#define BIG_BUFFER_SIZE DYNAMIC_JSON_DOCUMENT_SIZE
#else
#define MAKE_DOC(name) DynamicJsonDocument name(4096)
#define MAKE_BIG_DOC(name) DynamicJsonDocument name(100000)
#define BIG_BUFFER_SIZE 100000
#endif

int main() {
    AsyncWebServer server(0);

    // ---- JSON responses -------------------------------------------------------
    server.on("/obj", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncJsonResponse* response = new AsyncJsonResponse();
        JsonVariant& root = response->getRoot();
        root["name"] = request->arg("name");  // an Arduino String goes straight into the document
        root["n"] = 42;
        root["ok"] = true;
        root["list"][0] = 1;
        root["list"][1] = 2;
        response->setLength();
        request->send(response);
    });

    server.on("/arr", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncJsonResponse* response = new AsyncJsonResponse(true, BIG_BUFFER_SIZE);
        for (int i = 0; i < 3000; ++i) response->getRoot().add(i);  // large: many chunks
        response->setLength();
        request->send(response);
    });

    server.on("/pretty", HTTP_GET, [](AsyncWebServerRequest* request) {
        PrettyAsyncJsonResponse* response = new PrettyAsyncJsonResponse();
        response->getRoot()["a"] = 1;
        response->getRoot()["b"] = "two";
        response->setLength();
        request->send(response);
    });

    server.on("/forgot-length", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncJsonResponse* response = new AsyncJsonResponse();
        response->getRoot()["x"] = 1;
        request->send(response);  // no setLength(): the real library answers 500 as well
    });

    server.on("/msgpack", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncMessagePackResponse* response = new AsyncMessagePackResponse();
        response->getRoot()["a"] = 1;
        response->setLength();
        request->send(response);
    });

    // ---- the common ArduinoJson patterns: serialize into a response stream ---------
    server.on("/stream", HTTP_GET, [](AsyncWebServerRequest* request) {
        MAKE_DOC(doc);
        doc["via"] = "stream";
        doc["pi"] = 3.5;
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
    });

    // ---- JSON requests -----------------------------------------------------------
    auto* adder = new AsyncCallbackJsonWebHandler("/api/add", [](AsyncWebServerRequest* request, JsonVariant& json) {
        int sum = json["a"].as<int>() + json["b"].as<int>();
        String who = json["who"] | "nobody";
        MAKE_DOC(out);
        out["sum"] = sum;
        out["who"] = who;
        out["method"] = request->methodToString();
        String text;
        serializeJson(out, text);  // into an Arduino String
        request->send(200, "application/json", text);
    });
    adder->setMaxContentLength(200);
    server.addHandler(adder);

    server.addHandler(new AsyncCallbackJsonWebHandler("/api/echo", [](AsyncWebServerRequest* request, JsonVariant& json) {
        MAKE_DOC(out);
        out["items"] = json.size();
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        serializeJson(out, *response);
        request->send(response);
    }));

    server.onNotFound([](AsyncWebServerRequest* request) { request->send(404, "text/plain", "none"); });
    server.begin();
    uint16_t port = server.port();
    const std::string json = "Content-Type: application/json\r\n";

    // ===================== responses =====================
    {
        HttpResponse r = httpGet(port, "/obj?name=Ada%20L");
        CHECK(r.status == 200);
        CHECK(r.header("content-type") == "application/json");
        CHECK(!r.has("transfer-encoding"));  // length is known
        MAKE_DOC(doc);
        CHECK(!deserializeJson(doc, r.body));
        CHECK(String(doc["name"].as<const char*>()) == "Ada L");
        CHECK(doc["n"].as<int>() == 42);
        CHECK(doc["ok"].as<bool>());
        CHECK(doc["list"][1].as<int>() == 2);
        CHECK(r.header("content-length") == std::to_string(r.body.size()));

        r = httpGet(port, "/arr");
        MAKE_BIG_DOC(arr);
        CHECK(!deserializeJson(arr, r.body));
        CHECK(arr.size() == 3000);
        CHECK(arr[2999].as<int>() == 2999);

        r = httpGet(port, "/pretty");
        CHECK(r.body.find("\n") != std::string::npos && r.body.find("  \"a\": 1") != std::string::npos);

        CHECK(httpGet(port, "/forgot-length").status == 500);

        r = httpGet(port, "/msgpack");
        CHECK(r.header("content-type") == "application/x-msgpack");
        CHECK(r.body == std::string("\x81\xA1" "a\x01", 4));

        r = httpGet(port, "/stream");
        CHECK(r.chunked);  // a stream has no known length
        MAKE_DOC(s);
        CHECK(!deserializeJson(s, r.body));
        CHECK(String(s["via"].as<const char*>()) == "stream");
        CHECK(s["pi"].as<double>() == 3.5);
    }

    // ===================== request handler =====================
    {
        HttpResponse r = httpExchange(port, {httpReq("POST", "/api/add", json, "{\"a\":3,\"b\":4,\"who\":\"Bob\"}")});
        CHECK(r.status == 200);
        MAKE_DOC(doc);
        CHECK(!deserializeJson(doc, r.body));
        CHECK(doc["sum"].as<int>() == 7);
        CHECK(String(doc["who"].as<const char*>()) == "Bob");
        CHECK(String(doc["method"].as<const char*>()) == "POST");

        // PUT and PATCH are handled too; the Content-Type may carry a charset
        r = httpExchange(port, {httpReq("PUT", "/api/add", "Content-Type: application/json; charset=utf-8\r\n", "{\"a\":1,\"b\":1}")});
        CHECK(r.status == 200 && r.body.find("\"sum\":2") != std::string::npos);
        r = httpExchange(port, {httpReq("PATCH", "/api/add", json, "{\"a\":5,\"b\":5}")});
        CHECK(r.status == 200 && r.body.find("\"method\":\"PATCH\"") != std::string::npos);

        // default of a missing member
        r = httpExchange(port, {httpReq("POST", "/api/add", json, "{\"a\":1,\"b\":2}")});
        CHECK(r.body.find("\"who\":\"nobody\"") != std::string::npos);

        // anything that is not JSON is not this handler's business
        r = httpExchange(port, {httpReq("POST", "/api/add", "Content-Type: text/plain\r\n", "{\"a\":1}")});
        CHECK(r.status == 404);
        r = httpGet(port, "/api/add");  // GET is not in the method mask
        CHECK(r.status == 404);

        // malformed and empty bodies -> 400
        r = httpExchange(port, {httpReq("POST", "/api/add", json, "{not json")});
        CHECK(r.status == 400);
        r = httpExchange(port, {httpReq("POST", "/api/add", json, "")});
        CHECK(r.status == 400);

        // over the configured limit (200 bytes) -> 413 without buffering the body
        r = httpExchange(port, {httpReq("POST", "/api/add", json, "{\"a\":\"" + std::string(500, 'x') + "\"}")});
        CHECK(r.status == 413);

        // body split into tiny TCP pieces
        std::string req = httpReq("POST", "/api/add", json, "{\"a\":20,\"b\":22}");
        std::vector<std::string> pieces;
        for (size_t i = 0; i < req.size(); i += 4) pieces.push_back(req.substr(i, 4));
        r = httpExchange(port, pieces, 4000, 1);
        CHECK(r.status == 200 && r.body.find("\"sum\":42") != std::string::npos);

        // a handler with the default 16 KB limit takes a larger document
        std::string many = "[";
        for (int i = 0; i < 2000; ++i) many += (i ? "," : "") + std::to_string(i);
        many += "]";
        r = httpExchange(port, {httpReq("POST", "/api/echo", json, many)}, 5000);
        CHECK(r.status == 200 && r.body == "{\"items\":2000}");
    }

    // ===================== ArduinoJson with the emulated File / String =====================
    {
        stdfs::path root = stdfs::temp_directory_path() / ("espasyncemu_json_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        FS fs(root.string());
        fs.begin();
        {
            MAKE_DOC(doc);
            doc["wifi"] = "home";
            doc["port"] = 8080;
            File f = fs.open("/config.json", "w");
            CHECK(serializeJson(doc, f) > 0);  // File is a Print
        }
        {
            MAKE_DOC(doc);
            File f = fs.open("/config.json");
            CHECK(!deserializeJson(doc, f));   // File is a Stream
            CHECK(String(doc["wifi"].as<const char*>()) == "home");
            CHECK(doc["port"].as<int>() == 8080);
        }
        {
            MAKE_DOC(doc);
            String text = "{\"k\":\"v\"}";
            CHECK(!deserializeJson(doc, text));  // an Arduino String as input
            String out;
            serializeJson(doc, out);              // ...and as output
            CHECK(out == "{\"k\":\"v\"}");
        }
        stdfs::remove_all(root);
    }

    std::puts("json_test OK");
    return 0;
}
