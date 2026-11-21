#include "FS.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace stdfs = std::filesystem;

fs::FS LittleFS("./littlefs");
fs::FS SPIFFS("./spiffs");

namespace fs {

struct File::Impl {
    FILE* fp = nullptr;
    bool isDir = false;
    std::string path;      // device path ("/a/b.txt")
    std::string name;      // last component
    std::string hostPath;
    std::vector<std::string> entries;  // directory listing (device paths)
    size_t nextEntry = 0;
    const FS* owner = nullptr;
    std::string root;

    ~Impl() { if (fp) std::fclose(fp); }
};

// ---- path handling ---------------------------------------------------------
bool FS::hostPath(const char* path, std::string& out) const {
    if (!path) return false;
    std::string p = path;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string seg = p.substr(i, j - i);
        if (seg == "..") return false;  // no escaping the sandbox
        if (!seg.empty() && seg != ".") parts.push_back(seg);
        i = j + 1;
    }
    stdfs::path r(_root);
    for (auto& s : parts) r /= s;
    out = r.string();
    return true;
}

static std::string devicePath(const char* path) {
    std::string p = path ? path : "";
    std::string out;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string seg = p.substr(i, j - i);
        if (!seg.empty() && seg != ".") out += "/" + seg;
        i = j + 1;
    }
    return out.empty() ? "/" : out;
}

static std::string lastComponent(const std::string& devPath) {
    size_t p = devPath.find_last_of('/');
    return p == std::string::npos ? devPath : devPath.substr(p + 1);
}

// ---- FS ----------------------------------------------------------------------
bool FS::begin(bool) {
    std::error_code ec;
    stdfs::create_directories(_root, ec);
    return !ec;
}

bool FS::format() {
    std::error_code ec;
    for (auto& e : stdfs::directory_iterator(_root, ec)) stdfs::remove_all(e.path(), ec);
    return true;
}

File FS::open(const char* path, const char* mode) {
    std::string hp;
    if (!hostPath(path, hp)) return File();
    std::string m = mode ? mode : "r";
    std::error_code ec;

    auto impl = std::make_shared<File::Impl>();
    impl->path = devicePath(path);
    impl->name = lastComponent(impl->path);
    impl->hostPath = hp;
    impl->owner = this;
    impl->root = _root;

    if (m == "r") {
        if (stdfs::is_directory(hp, ec)) {
            impl->isDir = true;
            std::vector<std::string> names;
            for (auto& e : stdfs::directory_iterator(hp, ec)) names.push_back(e.path().filename().string());
            std::sort(names.begin(), names.end());
            std::string base = impl->path == "/" ? "" : impl->path;
            for (auto& n : names) impl->entries.push_back(base + "/" + n);
        } else {
            impl->fp = std::fopen(hp.c_str(), "rb");
            if (!impl->fp) return File();
        }
    } else {
        const char* cm = (m == "a") ? "ab" : (m == "r+") ? "r+b" : (m == "w+") ? "w+b" : "wb";
        stdfs::create_directories(stdfs::path(hp).parent_path(), ec);
        impl->fp = std::fopen(hp.c_str(), cm);
        if (!impl->fp) return File();
    }
    File f;
    f._i = impl;
    return f;
}

bool FS::exists(const char* path) {
    std::string hp;
    if (!hostPath(path, hp)) return false;
    std::error_code ec;
    return stdfs::exists(hp, ec);
}

bool FS::remove(const char* path) {
    std::string hp;
    if (!hostPath(path, hp)) return false;
    std::error_code ec;
    return stdfs::is_regular_file(hp, ec) && stdfs::remove(hp, ec);
}

bool FS::rename(const char* from, const char* to) {
    std::string a, b;
    if (!hostPath(from, a) || !hostPath(to, b)) return false;
    std::error_code ec;
    stdfs::rename(a, b, ec);
    return !ec;
}

bool FS::mkdir(const char* path) {
    std::string hp;
    if (!hostPath(path, hp)) return false;
    std::error_code ec;
    return stdfs::create_directories(hp, ec);
}

bool FS::rmdir(const char* path) {
    std::string hp;
    if (!hostPath(path, hp)) return false;
    std::error_code ec;
    return stdfs::is_directory(hp, ec) && stdfs::remove(hp, ec);  // only empty directories
}

// ---- File ----------------------------------------------------------------------
size_t File::read(uint8_t* buf, size_t size) { return (_i && _i->fp) ? std::fread(buf, 1, size, _i->fp) : 0; }

int File::read() {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
}

int File::peek() {
    if (!_i || !_i->fp) return -1;
    int c = std::fgetc(_i->fp);
    if (c != EOF) std::ungetc(c, _i->fp);
    return c == EOF ? -1 : c;
}

int File::available() {
    if (!_i || !_i->fp) return 0;
    size_t sz = size(), pos = position();
    return sz > pos ? static_cast<int>(sz - pos) : 0;
}

size_t File::write(const uint8_t* buf, size_t size) { return (_i && _i->fp) ? std::fwrite(buf, 1, size, _i->fp) : 0; }
void File::flush() { if (_i && _i->fp) std::fflush(_i->fp); }

bool File::seek(uint32_t pos, SeekMode mode) {
    if (!_i || !_i->fp) return false;
    int whence = mode == SeekSet ? SEEK_SET : mode == SeekCur ? SEEK_CUR : SEEK_END;
    return std::fseek(_i->fp, static_cast<long>(pos), whence) == 0;
}

size_t File::position() const { return (_i && _i->fp) ? static_cast<size_t>(std::ftell(_i->fp)) : 0; }

size_t File::size() const {
    if (!_i) return 0;
    std::error_code ec;
    if (_i->isDir) return 0;
    if (_i->fp) std::fflush(_i->fp);
    auto s = stdfs::file_size(_i->hostPath, ec);
    return ec ? 0 : static_cast<size_t>(s);
}

void File::close() { _i.reset(); }
const char* File::name() const { return _i ? _i->name.c_str() : ""; }
const char* File::path() const { return _i ? _i->path.c_str() : ""; }
bool File::isDirectory() const { return _i && _i->isDir; }

File File::openNextFile() {
    if (!_i || !_i->isDir || _i->nextEntry >= _i->entries.size() || !_i->owner) return File();
    const std::string& p = _i->entries[_i->nextEntry++];
    return const_cast<FS*>(_i->owner)->open(p.c_str(), "r");
}

time_t File::getLastWrite() const {
    if (!_i) return 0;
    std::error_code ec;
    auto t = stdfs::last_write_time(_i->hostPath, ec);
    if (ec) return 0;
    // file_time_type -> time_t without relying on C++20 clock_cast
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - stdfs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(sctp);
}

}  // namespace fs
