// An unmodified-style Arduino sketch: TCP + UDP echo on port 7007 and a
// periodic status line. Build target: echo_sketch.
#include <Arduino.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncUDP.h>

static const uint16_t kPort = 7007;

AsyncServer tcpServer(kPort);
AsyncUDP udp;
static uint32_t tcpBytes = 0, udpPackets = 0;

void setup() {
    Serial.begin(115200);
    Serial.println("echo sketch starting");

    tcpServer.onClient([](void*, AsyncClient* c) {
        c->onData([](void*, AsyncClient* cl, void* data, size_t len) {
            tcpBytes += static_cast<uint32_t>(len);
            cl->write(static_cast<const char*>(data), len);
        });
        c->onDisconnect([](void*, AsyncClient* cl) { delete cl; });
    });
    tcpServer.begin();

    if (udp.listen(kPort)) {
        udp.onPacket([](AsyncUDPPacket& p) {
            ++udpPackets;
            p.write(p.data(), p.length());  // echo to sender
        });
    }
    Serial.printf("listening on TCP+UDP %u\n", kPort);
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last >= 5000) {
        last = millis();
        Serial.printf("[%u ms] tcp bytes=%u udp packets=%u\n", millis(), tcpBytes, udpPackets);
    }
}
