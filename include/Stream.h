// Arduino-compatible Stream: a Print that can also be read. File derives from it,
// which is what lets ArduinoJson's deserializeJson(doc, file) read from a File.
#pragma once
#include "Print.h"

class Stream : public Print {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;

    virtual size_t readBytes(char* buffer, size_t length) {
        size_t n = 0;
        while (n < length) {
            int c = read();
            if (c < 0) break;
            buffer[n++] = static_cast<char>(c);
        }
        return n;
    }

    String readString() {
        String s;
        int c;
        while ((c = read()) >= 0) s += static_cast<char>(c);
        return s;
    }

    String readStringUntil(char terminator) {
        String s;
        int c;
        while ((c = read()) >= 0 && c != terminator) s += static_cast<char>(c);
        return s;
    }

    void setTimeout(unsigned long) {}
};
