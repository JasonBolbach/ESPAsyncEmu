// WiFi stub: the desktop is "always connected" at 127.0.0.1, so sketches that
// wait for WL_CONNECTED run through immediately.
#pragma once
#include "Arduino.h"

typedef enum {
    WL_NO_SHIELD = 255,
    WL_IDLE_STATUS = 0,
    WL_NO_SSID_AVAIL = 1,
    WL_SCAN_COMPLETED = 2,
    WL_CONNECTED = 3,
    WL_CONNECT_FAILED = 4,
    WL_CONNECTION_LOST = 5,
    WL_DISCONNECTED = 6
} wl_status_t;

typedef enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 } WiFiMode_t;

class WiFiClass {
public:
    bool mode(WiFiMode_t m) { _mode = m; return true; }
    WiFiMode_t getMode() const { return _mode; }
    wl_status_t begin(const char* ssid = nullptr, const char* = nullptr) {
        _ssid = ssid ? ssid : "";
        return WL_CONNECTED;
    }
    wl_status_t status() const { return WL_CONNECTED; }
    bool isConnected() const { return true; }
    bool disconnect(bool = false) { return true; }
    void setAutoReconnect(bool) {}
    void setSleep(bool) {}
    bool setHostname(const char*) { return true; }
    IPAddress localIP() const { return IPAddress(127, 0, 0, 1); }
    IPAddress gatewayIP() const { return IPAddress(127, 0, 0, 1); }
    IPAddress subnetMask() const { return IPAddress(255, 0, 0, 0); }
    String SSID() const { return _ssid; }
    String macAddress() const { return "00:00:00:00:00:00"; }
    int32_t RSSI() const { return -40; }
    bool softAP(const char*, const char* = nullptr, int = 1, int = 0, int = 4) { return true; }
    IPAddress softAPIP() const { return IPAddress(127, 0, 0, 1); }

private:
    WiFiMode_t _mode = WIFI_OFF;
    String _ssid;
};
inline WiFiClass WiFi;
