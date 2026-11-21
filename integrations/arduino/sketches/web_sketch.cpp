// A web server sketch written the way it would be for a real ESP8266/ESP32:
// WiFi, AsyncWebServer routes with query parameters, a PROGMEM page with
// %PLACEHOLDER% templating, a WebSocket, and Server-Sent Events.
//
//   ./web_sketch --port-offset 8000      # server(80) listens on 8080
//   open http://127.0.0.1:8080/
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>

const char* ssid = "my-network";
const char* password = "my-password";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncEventSource events("/events");

bool ledOn = false;
int counter = 0;

const char index_html[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>ESP sketch</title></head>
<body style="font-family:sans-serif">
<h1>ESPAsyncEmu web sketch</h1>
<p>LED is <b>%LED%</b> &middot; counter = <b id="c">%COUNTER%</b></p>
<p><a href="/led?state=1">LED on</a> &middot; <a href="/led?state=0">LED off</a> &middot; <a href="/api">JSON</a></p>
<pre id="log"></pre>
<script>
  const log = m => document.getElementById('log').textContent += m + '\n';
  const es = new EventSource('/events');
  es.addEventListener('tick', e => { document.getElementById('c').textContent = e.data; });
  const ws = new WebSocket('ws://' + location.host + '/ws');
  ws.onmessage = e => log('ws: ' + e.data);
  ws.onopen = () => ws.send('hello from the browser');
</script></body></html>)rawliteral";

String processor(const String& var) {
    if (var == "LED") return ledOn ? "ON" : "OFF";
    if (var == "COUNTER") return String(counter);
    return String();
}

void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf("ws[%u] connected\n", client->id());
        client->text("welcome");
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf("ws[%u] disconnected\n", client->id());
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo* info = static_cast<AwsFrameInfo*>(arg);
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            String msg(reinterpret_cast<char*>(data), len);
            Serial.printf("ws[%u] says: %s\n", client->id(), msg.c_str());
            srv->textAll(String("[") + client->id() + "] " + msg);
        }
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP().toString());

    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send_P(200, "text/html", index_html, processor);
    });

    server.on("/led", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (request->hasParam("state")) {
            ledOn = request->getParam("state")->value() == "1";
            digitalWrite(LED_BUILTIN, ledOn ? LOW : HIGH);  // active low on many boards
        }
        request->redirect("/");
    });

    server.on("/api", HTTP_GET, [](AsyncWebServerRequest* request) {
        String json = String("{\"led\":") + (ledOn ? "true" : "false") + ",\"counter\":" + counter +
                      ",\"uptime_ms\":" + millis() + ",\"heap\":" + ESP.getFreeHeap() + "}";
        request->send(200, "application/json", json);
    });

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.addHandler(&events);

    server.onNotFound([](AsyncWebServerRequest* request) { request->send(404, "text/plain", "Not found"); });
    server.begin();
    Serial.printf("HTTP server started on port %u\n", server.port());
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last >= 2000) {
        last = millis();
        ++counter;
        events.send(String(counter).c_str(), "tick", counter);
    }
    ws.cleanupClients();
}
