// Arduino-style file system emulation (FS / File, plus LittleFS and SPIFFS
// objects) backed by a directory on the host.
//
//   LittleFS.setRoot("./data");   // emulation extension: where "/" lives on the host
//   LittleFS.begin();             // creates the directory if needed
//   File f = LittleFS.open("/index.html");
//
// Paths are always rooted at the FS root; ".." components are rejected so
// device code (and uploaded filenames) can't escape the sandbox directory.
// Default roots: ./littlefs and ./spiffs relative to the working directory.
#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>

#include "Stream.h"
#include "WString.h"

namespace fs {

enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };

class FS;

class File : public Stream {
public:
    File() = default;
    explicit operator bool() const { return static_cast<bool>(_i); }

    size_t read(uint8_t* buf, size_t size);
    int read() override;
    int peek() override;
    int available() override;
    size_t write(const uint8_t* buf, size_t size) override;
    size_t write(uint8_t b) override { return write(&b, 1); }
    using Print::write;
    void flush() override;
    bool seek(uint32_t pos, SeekMode mode = SeekSet);
    size_t position() const;
    size_t size() const;
    void close();

    const char* name() const;  // last path component
    const char* path() const;  // full path inside the FS
    bool isDirectory() const;
    File openNextFile();       // iterate a directory (invalid File at the end)
    time_t getLastWrite() const;

    struct Impl;

private:
    friend class FS;
    std::shared_ptr<Impl> _i;
};

class FS {
public:
    explicit FS(std::string defaultRoot = "./fs") : _root(std::move(defaultRoot)) {}

    // Emulation extension: choose the host directory that backs "/".
    void setRoot(const std::string& hostDir) { _root = hostDir; }
    const std::string& root() const { return _root; }

    bool begin(bool formatOnFail = false);  // creates the root directory if missing
    void end() {}
    bool format();                          // empties the root directory

    File open(const char* path, const char* mode = "r");
    File open(const String& path, const char* mode = "r") { return open(path.c_str(), mode); }
    bool exists(const char* path);
    bool exists(const String& path) { return exists(path.c_str()); }
    bool remove(const char* path);
    bool remove(const String& path) { return remove(path.c_str()); }
    bool rename(const char* from, const char* to);
    bool mkdir(const char* path);
    bool rmdir(const char* path);

    // Resolve a device path to a host path. Returns false for unsafe paths.
    bool hostPath(const char* path, std::string& out) const;

private:
    std::string _root;
};

}  // namespace fs

using fs::File;
using fs::FS;
using fs::SeekMode;

extern fs::FS LittleFS;
extern fs::FS SPIFFS;

#ifndef FILE_READ
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
#endif
