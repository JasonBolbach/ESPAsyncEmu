#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "ESPAsyncWebServer.h"
#include "WebUtil.h"
#include "http_test_util.h"

namespace stdfs = std::filesystem;

static void writeFile(const stdfs::path& p, const std::string& content) {
    stdfs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

int main() {
    AsyncWebServer server(0);
    bool wrongThread = false;
    auto mainThread = std::this_thread::get_id();
    (void)mainThread;

    // ---- routes ---------------------------------------------------------------
    server.on("/hello", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "Hello"); });

    server.on("/post-only", HTTP_POST, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "posted"); });

    server.on("/any", [](AsyncWebServerRequest* r) {
        r->send(200, "text/plain", String(r->methodToString()) + " " + r->url());
    });

    server.on("/args", HTTP_ANY, [](AsyncWebServerRequest* r) {
        String out;
        out += "params=" + String(r->params()) + ";";
        out += "x=" + r->arg("x") + ";";
        out += "y=" + r->arg("y") + ";";
        out += "flag=" + String(r->hasArg("flag") ? 1 : 0) + ";";
        AsyncWebParameter* p = r->getParam("p", true);
        out += "p=" + String(p ? p->value() : String("-")) + ";";
        out += "ispost=" + String(p && p->isPost() ? 1 : 0) + ";";
        out += "host=" + r->host() + ";ct=" + r->contentType() + ";";
        r->send(200, "text/plain", out);
    });

    // raw body (JSON etc.) streamed to onBody
    std::string bodyGot;
    size_t bodyCalls = 0, bodyTotal = 0;
    server.on(
        "/body", HTTP_POST,
        [&](AsyncWebServerRequest* r) {
            r->send(200, "text/plain", String(bodyGot.size()) + ":" + String(bodyCalls > 0 ? 1 : 0));
        },
        nullptr,
        [&](AsyncWebServerRequest*, uint8_t* data, size_t len, size_t index, size_t total) {
            EXPECT(index == bodyGot.size());
            bodyGot.append(reinterpret_cast<char*>(data), len);
            bodyTotal = total;
            ++bodyCalls;
        });

    // multipart upload
    std::string uploadData, uploadName;
    int uploadFinals = 0;
    size_t uploadCalls = 0;
    server.on(
        "/upload", HTTP_POST,
        [&](AsyncWebServerRequest* r) {
            AsyncWebParameter* f = r->getParam("file", true, true);
            AsyncWebParameter* t = r->getParam("title", true, false);
            String out = String("file=") + (f ? f->value() : String("-")) + ";size=" + String(f ? f->size() : 0) + ";title=" + (t ? t->value() : String("-")) +
                         ";multipart=" + String(r->multipart() ? 1 : 0);
            r->send(200, "text/plain", out);
        },
        [&](AsyncWebServerRequest*, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
            EXPECT(index == uploadData.size());
            uploadName = filename;
            uploadData.append(reinterpret_cast<char*>(data), len);
            ++uploadCalls;
            if (final) ++uploadFinals;
        });

    // responses
    server.on("/chunked", [](AsyncWebServerRequest* r) {
        r->sendChunked("text/plain", [](uint8_t* buf, size_t max, size_t index) -> size_t {
            (void)max;
            if (index >= 6 * 5) return 0;
            std::memcpy(buf, ("chunk" + std::to_string(index / 6)).c_str(), 6);
            return 6;
        });
    });

    server.on("/big", [](AsyncWebServerRequest* r) {
        const size_t total = 50000;
        r->send("application/octet-stream", total, [total](uint8_t* buf, size_t max, size_t index) -> size_t {
            size_t n = std::min(max, total - index);
            for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>((index + i) % 251);
            return n;
        });
    });

    server.on("/stream", [](AsyncWebServerRequest* r) {
        AsyncResponseStream* s = r->beginResponseStream("text/plain");
        s->print("line1\n");
        s->printf("n=%d s=%s\n", 42, "str");
        s->print(std::string(3000, 'z'));
        r->send(s);
    });

    server.on("/template", [](AsyncWebServerRequest* r) {
        static const std::string src = "Hello %NAME%! 100% sure, %%x%% [%missing%] end";
        r->send("text/plain", 0,
                [](uint8_t* buf, size_t max, size_t index) -> size_t {
                    size_t n = std::min<size_t>({max, 3, src.size() - std::min(index, src.size())});  // 3 bytes at a time: forces splits
                    std::memcpy(buf, src.data() + index, n);
                    return n;
                },
                [](const String& name) -> String { return name == "NAME" ? String("World") : String(); });
    });

    server.on("/progmem", [](AsyncWebServerRequest* r) {
        r->send_P(200, "text/plain", "id=%ID%", [](const String& n) { return n == "ID" ? String("7") : String(); });
    });

    server.on("/redirect", [](AsyncWebServerRequest* r) { r->redirect("/hello"); });

    server.on("/headers", [](AsyncWebServerRequest* r) {
        AsyncWebServerResponse* resp = r->beginResponse(200, "text/plain", String(r->header("X-Custom")) + "|" + r->header("x-custom"));
        resp->addHeader("X-Reply", "yes");
        r->send(resp);
    });

    server.on("/status", [](AsyncWebServerRequest* r) { r->send(204); });

    // regex route with captures
    server.on("^\\/user\\/([0-9]+)\\/post\\/([a-z]+)$", HTTP_GET, [](AsyncWebServerRequest* r) {
        r->send(200, "text/plain", r->pathArg(0) + "-" + r->pathArg(1));
    });

    // wildcard + prefix routes
    server.on("/wild/*", [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "wild:" + r->url()); });
    server.on("/prefix", [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "prefix:" + r->url()); });

    // rewrite
    server.rewrite("/old", "/args?x=rewritten");

    // deferred response
    AsyncWebServerRequest* deferred = nullptr;
    server.on("/later", [&](AsyncWebServerRequest* r) {
        deferred = r;
        AsyncEmu::post([&] {
            if (deferred) deferred->send(200, "text/plain", "late");
        });
    });

    // disconnect callback when the client leaves before we answer
    bool sawDisconnect = false;
    server.on("/never", [&](AsyncWebServerRequest* r) { r->onDisconnect([&] { sawDisconnect = true; }); });

    // authentication
    server.on("/basic", [](AsyncWebServerRequest* r) {
        if (!r->authenticate("user", "pass")) return r->requestAuthentication("Test", false);
        r->send(200, "text/plain", "basic-ok");
    });
    server.on("/digest", [](AsyncWebServerRequest* r) {
        if (!r->authenticate("user", "pass", "Realm")) return r->requestAuthentication("Realm");
        r->send(200, "text/plain", "digest-ok");
    });
    server.on("/handler-auth", [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "inner"); }).setAuthentication("a", "b");

    // filters
    server.on("/sta", [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "sta"); }).setFilter(ON_STA_FILTER);
    server.on("/ap", [](AsyncWebServerRequest* r) { r->send(200, "text/plain", "ap"); }).setFilter(ON_AP_FILTER);

    // static files
    stdfs::path root = stdfs::temp_directory_path() / ("espasyncemu_web_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    stdfs::remove_all(root);
    FS testFs(root.string());
    CHECK(testFs.begin());
    writeFile(root / "www/index.htm", "<h1>root</h1>");
    writeFile(root / "www/a.txt", "file a");
    writeFile(root / "www/style.css", "body{}");
    writeFile(root / "www/sub/index.htm", "<h1>sub</h1>");
    writeFile(root / "www/only-gz.txt.gz", "GZDATA");
    writeFile(root / "www/tmpl.txt", "v=%VER%");
    writeFile(root / "www/pic.png", "%VER%");  // binary type: the template processor must leave it alone
    writeFile(root / "secret.txt", "TOP SECRET");  // outside the served directory
    server.serveStatic("/static", testFs, "/www/", "max-age=600");
    server.serveStatic("/t", testFs, "/www/").setTemplateProcessor([](const String& n) { return n == "VER" ? String("1.2") : String(); });
    server.serveStatic("/lm", testFs, "/www/").setLastModified("Wed, 21 Oct 2015 07:28:00 GMT");

    // upload handled by the catch-all when no route claims it
    server.onNotFound([](AsyncWebServerRequest* r) { r->send(404, "text/plain", "custom-404:" + r->url()); });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

    server.begin();
    uint16_t port = server.port();
    CHECK(port != 0);

    // ===================== basic routing =====================
    {
        HttpResponse r = httpGet(port, "/hello");
        CHECK(r.completed);
        CHECK(r.status == 200);
        CHECK(r.body == "Hello");
        CHECK(r.header("content-type") == "text/plain");
        CHECK(r.header("content-length") == "5");
        CHECK(r.header("connection") == "close");
        CHECK(r.header("access-control-allow-origin") == "*");
        CHECK(r.statusLine == "HTTP/1.1 200 OK");

        r = httpGet(port, "/hello", "", "HTTP/1.0");
        CHECK(r.statusLine == "HTTP/1.0 200 OK");
        CHECK(r.body == "Hello");

        r = httpGet(port, "/nope");
        CHECK(r.status == 404);
        CHECK(r.body == "custom-404:/nope");

        r = httpExchange(port, {httpReq("GET", "/post-only")});
        CHECK(r.status == 404);  // wrong method falls through to the catch-all
        r = httpExchange(port, {httpReq("POST", "/post-only")});
        CHECK(r.status == 200 && r.body == "posted");

        r = httpExchange(port, {httpReq("DELETE", "/any")});
        CHECK(r.body == "DELETE /any");
        r = httpExchange(port, {httpReq("PUT", "/any")});
        CHECK(r.body == "PUT /any");
        r = httpExchange(port, {httpReq("PATCH", "/any")});
        CHECK(r.body == "PATCH /any");
        r = httpExchange(port, {httpReq("OPTIONS", "/any")});
        CHECK(r.body == "OPTIONS /any");

        // HEAD: headers (incl. Content-Length) but no body
        r = httpExchange(port, {httpReq("HEAD", "/any")});
        CHECK(r.status == 200);
        CHECK(r.header("content-length") == std::to_string(std::string("HEAD /any").size()));
        CHECK(r.body.empty());

        // route matching rules
        r = httpGet(port, "/wild/a/b");
        CHECK(r.body == "wild:/wild/a/b");
        r = httpGet(port, "/prefix/deeper/path");
        CHECK(r.body == "prefix:/prefix/deeper/path");
        r = httpGet(port, "/prefixed");  // "/prefix" must not match "/prefixed"
        CHECK(r.status == 404);

        r = httpGet(port, "/user/42/post/abc");
        CHECK(r.body == "42-abc");
        r = httpGet(port, "/user/x/post/abc");
        CHECK(r.status == 404);

        // percent-decoded path
        r = httpGet(port, "/any%2Fx");
        CHECK(r.status == 404 || r.body.find("/any/x") != std::string::npos);

        // filters
        CHECK(httpGet(port, "/sta").body == "sta");
        CHECK(httpGet(port, "/ap").status == 404);

        // rewrite
        r = httpGet(port, "/old");
        CHECK(r.body.find("x=rewritten;") != std::string::npos);

        // status-only and redirect
        r = httpGet(port, "/status");
        CHECK(r.status == 204);
        CHECK(!r.has("content-length"));
        CHECK(r.body.empty());
        r = httpGet(port, "/redirect");
        CHECK(r.status == 302);
        CHECK(r.header("location") == "/hello");
    }

    // ===================== params and headers =====================
    {
        HttpResponse r = httpGet(port, "/args?x=1&y=a%20b+c&flag");
        CHECK(r.body.find("params=3;") == 0);
        CHECK(r.body.find("x=1;") != std::string::npos);
        CHECK(r.body.find("y=a b c;") != std::string::npos);
        CHECK(r.body.find("flag=1;") != std::string::npos);
        CHECK(r.body.find("host=localhost;") != std::string::npos);

        // urlencoded POST body
        r = httpExchange(port, {httpReq("POST", "/args?x=q", "Content-Type: application/x-www-form-urlencoded\r\n", "p=hello%21+world&y=2")});
        CHECK(r.body.find("p=hello! world;") != std::string::npos);
        CHECK(r.body.find("ispost=1;") != std::string::npos);
        CHECK(r.body.find("x=q;") != std::string::npos);
        CHECK(r.body.find("ct=application/x-www-form-urlencoded;") != std::string::npos);

        // custom request header, case-insensitive; custom response header
        r = httpGet(port, "/headers", "X-Custom: value 1\r\n");
        CHECK(r.body == "value 1|value 1");
        CHECK(r.header("x-reply") == "yes");
    }

    // ===================== request bodies =====================
    {
        std::string json(20000, 'j');
        for (size_t i = 0; i < json.size(); ++i) json[i] = static_cast<char>('a' + i % 26);
        bodyGot.clear();
        HttpResponse r = httpExchange(port, {httpReq("POST", "/body", "Content-Type: application/json\r\n", json)});
        CHECK(r.status == 200);
        CHECK(bodyGot == json);
        CHECK(bodyTotal == json.size());
        CHECK(r.body == "20000:1");

        // Expect: 100-continue
        bodyGot.clear();
        AsyncClient c;
        std::string raw;
        bool up = false, closed = false;
        c.onConnect([&](void*, AsyncClient*) { up = true; });
        c.onData([&](void*, AsyncClient*, void* d, size_t n) { raw.append(static_cast<char*>(d), n); });
        c.onDisconnect([&](void*, AsyncClient*) { closed = true; });
        c.connect(IPAddress(127, 0, 0, 1), port);
        CHECK(pumpUntil([&] { return up; }));
        std::string head = "POST /body HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n";
        c.write(head.data(), head.size());
        CHECK(pumpUntil([&] { return raw.find("100 Continue") != std::string::npos; }));
        c.write("hello", 5);
        CHECK(pumpUntil([&] { return closed; }));
        CHECK(raw.find("HTTP/1.1 200 OK") != std::string::npos);
        CHECK(bodyGot == "hello");
    }

    // ===================== multipart upload =====================
    {
        std::string b = "----WebKitFormBoundaryX7";
        std::string file = pseudoRandomBytes(100000);
        // content that looks like the start of a delimiter, to test partial-match handling
        file.replace(5000, 10, "\r\n----WebKi");
        file.replace(70000, 6, "\r\n--\r\n");
        std::string body = "--" + b + "\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nMy Title\r\n" +
                           "--" + b + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"data.bin\"\r\n" +
                           "Content-Type: application/octet-stream\r\n\r\n" + file + "\r\n--" + b + "--\r\n";
        uploadData.clear();
        uploadFinals = 0;
        uploadCalls = 0;
        HttpResponse r = httpExchange(port, {httpReq("POST", "/upload", "Content-Type: multipart/form-data; boundary=" + b + "\r\n", body)}, 6000);
        CHECK(r.status == 200);
        CHECK(r.body == "file=data.bin;size=" + std::to_string(file.size()) + ";title=My Title;multipart=1");
        CHECK(uploadName == "data.bin");
        CHECK(uploadData == file);
        CHECK(uploadFinals == 1);
        CHECK(uploadCalls > 1);  // streamed, not buffered

        // tiny pieces: boundaries and headers get split across TCP reads
        std::string small = "--" + b + "\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nT\r\n" + "--" + b +
                            "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"s.txt\"\r\n\r\nabc\r\n--" + b + "--\r\n";
        std::vector<std::string> pieces;
        std::string req = httpReq("POST", "/upload", "Content-Type: multipart/form-data; boundary=" + b + "\r\n", small);
        for (size_t i = 0; i < req.size(); i += 3) pieces.push_back(req.substr(i, 3));
        uploadData.clear();
        uploadFinals = 0;
        r = httpExchange(port, pieces, 8000, 1);
        CHECK(r.body == "file=s.txt;size=3;title=T;multipart=1");
        CHECK(uploadData == "abc");
        CHECK(uploadFinals == 1);

        // quoted boundary
        std::string qb = "xyz";
        std::string qbody = "--" + qb + "\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nQ\r\n--" + qb + "--\r\n";
        r = httpExchange(port, {httpReq("POST", "/upload", "Content-Type: multipart/form-data; boundary=\"" + qb + "\"\r\n", qbody)});
        CHECK(r.body.find("title=Q;") != std::string::npos);
    }

    // ===================== responses =====================
    {
        HttpResponse r = httpGet(port, "/chunked");
        CHECK(r.chunked);
        CHECK(r.body == "chunk0chunk1chunk2chunk3chunk4");
        CHECK(!r.has("content-length"));

        // HTTP/1.0: no chunking, body delimited by the connection close
        r = httpGet(port, "/chunked", "", "HTTP/1.0");
        CHECK(!r.chunked);
        CHECK(r.body == "chunk0chunk1chunk2chunk3chunk4");

        // known length, larger than the send buffer: exercises ack-driven flow control
        r = httpGet(port, "/big");
        CHECK(r.header("content-length") == "50000");
        CHECK(r.body.size() == 50000);
        bool patternOk = true;
        for (size_t i = 0; i < r.body.size(); ++i) patternOk = patternOk && static_cast<uint8_t>(r.body[i]) == i % 251;
        CHECK(patternOk);

        r = httpGet(port, "/stream");
        CHECK(r.chunked);
        CHECK(r.body == "line1\nn=42 s=str\n" + std::string(3000, 'z'));

        // template processing, with a source that yields 3 bytes at a time
        r = httpGet(port, "/template");
        CHECK(r.body == "Hello World! 100% sure, %x% [] end");
        r = httpGet(port, "/progmem");
        CHECK(r.body == "id=7");
    }

    // ===================== static files =====================
    {
        HttpResponse r = httpGet(port, "/static/a.txt");
        CHECK(r.status == 200);
        CHECK(r.body == "file a");
        CHECK(r.header("content-type") == "text/plain");
        CHECK(r.header("cache-control") == "max-age=600");
        std::string etag = r.header("etag");
        CHECK(!etag.empty());

        r = httpGet(port, "/static/style.css");
        CHECK(r.header("content-type") == "text/css");

        // conditional request -> 304
        r = httpGet(port, "/static/a.txt", "If-None-Match: " + etag + "\r\n");
        CHECK(r.status == 304);
        CHECK(r.body.empty());
        r = httpGet(port, "/static/a.txt", "If-None-Match: \"different\"\r\n");
        CHECK(r.status == 200);

        // default files
        r = httpGet(port, "/static/");
        CHECK(r.body == "<h1>root</h1>");
        CHECK(r.header("content-type") == "text/html");
        r = httpGet(port, "/static");
        CHECK(r.body == "<h1>root</h1>");
        r = httpGet(port, "/static/sub/");
        CHECK(r.body == "<h1>sub</h1>");

        // gzip variant
        r = httpGet(port, "/static/only-gz.txt");
        CHECK(r.status == 200);
        CHECK(r.body == "GZDATA");
        CHECK(r.header("content-encoding") == "gzip");
        CHECK(r.header("content-type") == "text/plain");

        // missing file and path traversal must not leak anything
        CHECK(httpGet(port, "/static/missing.txt").status == 404);
        r = httpGet(port, "/static/../secret.txt");
        CHECK(r.status == 404);
        CHECK(r.body.find("TOP SECRET") == std::string::npos);
        r = httpGet(port, "/static/%2e%2e/secret.txt");
        CHECK(r.body.find("TOP SECRET") == std::string::npos);

        // template processor on static files
        r = httpGet(port, "/t/tmpl.txt");
        CHECK(r.body == "v=1.2");
        r = httpGet(port, "/t/pic.png");  // not text: untouched, and cacheable
        CHECK(r.body == "%VER%");
        CHECK(r.has("etag"));
        CHECK(r.header("content-length") == "5");

        // Last-Modified / If-Modified-Since
        r = httpGet(port, "/lm/a.txt");
        CHECK(r.header("last-modified") == "Wed, 21 Oct 2015 07:28:00 GMT");
        r = httpGet(port, "/lm/a.txt", "If-Modified-Since: Wed, 21 Oct 2015 07:28:00 GMT\r\n");
        CHECK(r.status == 304);
    }

    // ===================== authentication =====================
    {
        HttpResponse r = httpGet(port, "/basic");
        CHECK(r.status == 401);
        CHECK(r.header("www-authenticate") == "Basic realm=\"Test\"");
        r = httpGet(port, "/basic", "Authorization: Basic " + AsyncEmu::web::base64Encode("user:pass") + "\r\n");
        CHECK(r.status == 200 && r.body == "basic-ok");
        r = httpGet(port, "/basic", "Authorization: Basic " + AsyncEmu::web::base64Encode("user:wrong") + "\r\n");
        CHECK(r.status == 401);

        // handler-level credentials
        CHECK(httpGet(port, "/handler-auth").status == 401);
        r = httpGet(port, "/handler-auth", "Authorization: Basic " + AsyncEmu::web::base64Encode("a:b") + "\r\n");
        CHECK(r.status == 200 && r.body == "inner");

        // digest: challenge, then a correctly computed response
        r = httpGet(port, "/digest");
        CHECK(r.status == 401);
        std::string ch = r.header("www-authenticate");
        CHECK(ch.compare(0, 7, "Digest ") == 0);
        auto field = [&](const std::string& key) {
            size_t p = ch.find(key + "=\"");
            if (p == std::string::npos) return std::string();
            p += key.size() + 2;
            return ch.substr(p, ch.find('"', p) - p);
        };
        std::string nonce = field("nonce"), opaque = field("opaque");
        CHECK(field("realm") == "Realm");
        CHECK(nonce.size() == 32);
        using AsyncEmu::web::md5Hex;
        std::string ha1 = md5Hex("user:Realm:pass"), ha2 = md5Hex("GET:/digest");
        std::string response = md5Hex(ha1 + ":" + nonce + ":00000001:abc123:auth:" + ha2);
        std::string auth = "Authorization: Digest username=\"user\", realm=\"Realm\", nonce=\"" + nonce + "\", uri=\"/digest\", "
                           "cnonce=\"abc123\", nc=00000001, qop=auth, response=\"" + response + "\", opaque=\"" + opaque + "\"\r\n";
        r = httpGet(port, "/digest", auth);
        CHECK(r.status == 200 && r.body == "digest-ok");
        // wrong password hash -> rejected
        std::string bad = auth;
        bad.replace(bad.find(response), response.size(), std::string(32, '0'));
        CHECK(httpGet(port, "/digest", bad).status == 401);
    }

    // ===================== lifecycle: deferred, disconnect, errors =====================
    {
        HttpResponse r = httpGet(port, "/later");
        CHECK(r.status == 200 && r.body == "late");
        deferred = nullptr;

        // client disconnects before the server answers
        {
            AsyncClient c;
            bool up = false;
            c.onConnect([&](void*, AsyncClient*) { up = true; });
            c.connect(IPAddress(127, 0, 0, 1), port);
            CHECK(pumpUntil([&] { return up; }));
            std::string rq = httpReq("GET", "/never");
            c.write(rq.data(), rq.size());
            pumpUntil([&] { return false; }, 100);  // just pump so the server sees the request
            c.close();
            CHECK(pumpUntil([&] { return sawDisconnect; }));
        }

        // malformed / unsupported requests
        r = httpExchange(port, {"GARBAGE\r\n\r\n"});
        CHECK(r.status == 400);
        r = httpExchange(port, {"BREW /pot HTTP/1.1\r\n\r\n"});
        CHECK(r.status == 501);
        r = httpExchange(port, {"GET /hello HTTP/2.0\r\n\r\n"});
        CHECK(r.status == 505);
        r = httpExchange(port, {"GET /hello HTTP/1.1\r\nX-Big: " + std::string(20000, 'a') + "\r\n\r\n"});
        CHECK(r.status == 431);
        r = httpExchange(port, {httpReq("POST", "/args", "Content-Type: application/x-www-form-urlencoded\r\n", std::string(1100000, 'a'))}, 8000);
        CHECK(r.status == 413);

        // leading blank line before the request line is tolerated
        r = httpExchange(port, {"\r\nGET /hello HTTP/1.1\r\nHost: x\r\n\r\n"});
        CHECK(r.body == "Hello");

        // bare LF line endings
        r = httpExchange(port, {"GET /hello HTTP/1.1\nHost: x\n\n"});
        CHECK(r.body == "Hello");

        // absolute-form request target
        r = httpExchange(port, {"GET http://localhost/hello HTTP/1.1\r\nHost: x\r\n\r\n"});
        CHECK(r.body == "Hello");

        // request split across many TCP segments
        std::vector<std::string> pieces;
        std::string rq = httpReq("GET", "/args?x=split", "X-A: 1\r\n");
        for (size_t i = 0; i < rq.size(); i += 2) pieces.push_back(rq.substr(i, 2));
        r = httpExchange(port, pieces, 6000, 1);
        CHECK(r.body.find("x=split;") != std::string::npos);

        // many requests in a row all succeed (each on its own connection)
        for (int i = 0; i < 20; ++i) CHECK(httpGet(port, "/hello").body == "Hello");
    }

    // ===================== request timeout =====================
    {
        AsyncWebServer s2(0);
        s2.setRequestTimeout(1);
        s2.begin();
        AsyncClient c;
        bool up = false, closed = false;
        c.onConnect([&](void*, AsyncClient*) { up = true; });
        c.onDisconnect([&](void*, AsyncClient*) { closed = true; });
        c.connect(IPAddress(127, 0, 0, 1), s2.port());
        CHECK(pumpUntil([&] { return up; }));
        c.write("GET /partial HTT", 16);  // never completed
        CHECK(pumpUntil([&] { return closed; }, 3000));
    }

    // ===================== server teardown with live connections =====================
    {
        auto* s3 = new AsyncWebServer(0);
        s3->on("/x", [](AsyncWebServerRequest*) { /* never answers */ });
        s3->begin();
        AsyncClient c;
        bool up = false, closed = false;
        c.onConnect([&](void*, AsyncClient*) { up = true; });
        c.onDisconnect([&](void*, AsyncClient*) { closed = true; });
        c.connect(IPAddress(127, 0, 0, 1), s3->port());
        CHECK(pumpUntil([&] { return up; }));
        std::string rq = httpReq("GET", "/x");
        c.write(rq.data(), rq.size());
        pumpUntil([&] { return false; }, 100);
        delete s3;  // closes the live connection, frees the request
        CHECK(pumpUntil([&] { return closed; }));
    }

    CHECK(!wrongThread);
    stdfs::remove_all(root);
    CHECK(!testFailed());
    std::puts("web_test OK");
    return 0;
}
