// Arduino compatibility layers: String (WString.h) and FS/LittleFS (FS.h).
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "Arduino.h"
#include "FS.h"
#include "Print.h"
#include "Stream.h"
#include "WString.h"
#include "test_util.h"

namespace stdfs = std::filesystem;

int main() {
    // ===================== String =====================
    {
        String s = "Hello";
        CHECK(s.length() == 5);
        s += ", ";
        s += String("World");
        s += '!';
        CHECK(s == "Hello, World!");
        CHECK(s.startsWith("Hello"));
        CHECK(s.endsWith("World!"));
        CHECK(!s.startsWith("World"));
        CHECK(s.startsWith("World", 7));
        CHECK(s.indexOf('W') == 7);
        CHECK(s.indexOf("World") == 7);
        CHECK(s.indexOf('z') == -1);
        CHECK(s.indexOf('o', 5) == 8);
        CHECK(s.lastIndexOf('o') == 8);
        CHECK(s.substring(7) == "World!");
        CHECK(s.substring(0, 5) == "Hello");
        CHECK(s.substring(99) == "");
        CHECK(s.charAt(1) == 'e');
        CHECK(String("  padded \t").length() == 10);

        String t = "  padded \t\n";
        t.trim();
        CHECK(t == "padded");

        String r = "a-b-c";
        r.replace("-", "+");
        CHECK(r == "a+b+c");
        r.replace('+', '_');
        CHECK(r == "a_b_c");
        r.remove(1, 2);
        CHECK(r == "a_c");

        String c = "MiXed";
        c.toLowerCase();
        CHECK(c == "mixed");
        c.toUpperCase();
        CHECK(c == "MIXED");
        CHECK(String("abc").equalsIgnoreCase("ABC"));
        CHECK(!String("abc").equalsIgnoreCase("ABD"));
        CHECK(String("abc").equals("abc"));

        CHECK(String("  42abc").toInt() == 42);
        CHECK(String("-7").toInt() == -7);
        CHECK(String("3.5").toFloat() == 3.5f);
        CHECK(String(42) == "42");
        CHECK(String(-5) == "-5");
        CHECK(String(255, 16) == "ff");
        CHECK(String(5u, 2) == "101");
        CHECK(String(3.14159, 2) == "3.14");
        CHECK(String(1.0f, 0) == "1");
        CHECK(String('x') == "x");
        CHECK(String((const char*)nullptr) == "");

        // mixing with std::string and C strings
        std::string std = "abc";
        String fromStd = std;
        std::string back = fromStd;  // String is-a std::string
        CHECK(back == "abc");
        CHECK(String("a") + "b" == "ab");
        CHECK("a" + String("b") == "ab");
        CHECK(String("a") + String("b") + 'c' == "abc");
        CHECK(String("n=") + std == "n=abc");
        CHECK(std + String("!") == "abc!");
        // numbers append their text (a plain char only appends that character)
        CHECK(String("n=") + 5 == "n=5");
        CHECK(String("n=") + 5u == "n=5");
        CHECK(String("n=") + (uint32_t)4000000000u == "n=4000000000");
        CHECK(String("n=") + (size_t)7 == "n=7");
        CHECK(String("n=") + (long)-3 == "n=-3");
        CHECK(String("n=") + 2.5 == "n=2.50");
        CHECK(String("n=") + (uint8_t)200 == "n=200");
        CHECK(String("n=") + 'c' == "n=c");
        CHECK(5 + String("x") == "5x");
        String acc = "v";
        acc += 12;
        acc += 'c';
        acc += 1.5f;
        CHECK(acc == "v12c1.50");
        String rv = String("x") + std::string("y");
        CHECK(rv == "xy");
        CHECK(String("k") != "v");
        CHECK(String("a") < String("b"));
    }

    // ===================== FS =====================
    {
        stdfs::path root = stdfs::temp_directory_path() / ("espasyncemu_fs_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        FS fs(root.string());
        CHECK(fs.begin());
        CHECK(stdfs::is_directory(root));

        // write, read back
        {
            File f = fs.open("/dir/a.txt", "w");  // parent directory is created
            CHECK(f);
            CHECK(f.write(reinterpret_cast<const uint8_t*>("hello"), 5) == 5);
            f.print(" world");
            f.println("!");
            f.close();
        }
        CHECK(fs.exists("/dir/a.txt"));
        CHECK(fs.exists("/dir"));
        CHECK(!fs.exists("/nope"));
        {
            File f = fs.open("/dir/a.txt");
            CHECK(f);
            CHECK(f.size() == 14);  // "hello world!\r\n": println ends lines with CRLF, like Arduino
            CHECK(f.available() == 14);
            CHECK(!f.isDirectory());
            CHECK(std::string(f.name()) == "a.txt");
            CHECK(std::string(f.path()) == "/dir/a.txt");
            CHECK(f.peek() == 'h');
            CHECK(f.read() == 'h');
            CHECK(f.position() == 1);
            CHECK(f.seek(6));
            CHECK(f.readString() == "world!\r\n");
            CHECK(f.available() == 0);
            CHECK(f.read() == -1);
            CHECK(f.getLastWrite() > 1600000000);
        }

        // append
        {
            File f = fs.open("/dir/a.txt", "a");
            f.print("more");
        }
        CHECK(fs.open("/dir/a.txt").size() == 18);

        // missing file
        CHECK(!fs.open("/missing.txt"));

        // directory listing
        fs.open("/dir/b.txt", "w").print("b");
        {
            File d = fs.open("/dir");
            CHECK(d.isDirectory());
            int n = 0;
            std::string names;
            for (File e = d.openNextFile(); e; e = d.openNextFile()) { ++n; names += std::string(e.name()) + ","; }
            CHECK(n == 2);
            CHECK(names == "a.txt,b.txt,");
        }

        // rename / remove / mkdir / rmdir
        CHECK(fs.rename("/dir/b.txt", "/dir/c.txt"));
        CHECK(!fs.exists("/dir/b.txt") && fs.exists("/dir/c.txt"));
        CHECK(fs.remove("/dir/c.txt"));
        CHECK(!fs.exists("/dir/c.txt"));
        CHECK(!fs.remove("/dir"));  // remove() is for files
        CHECK(fs.mkdir("/made/deep"));
        CHECK(fs.exists("/made/deep"));
        CHECK(fs.rmdir("/made/deep"));

        // sandbox: nothing outside the root is reachable
        {
            std::ofstream((root.parent_path() / "outside_secret.txt").string()) << "secret";
            CHECK(!fs.exists("/../outside_secret.txt"));
            CHECK(!fs.open("/../outside_secret.txt"));
            CHECK(!fs.open("/dir/../../outside_secret.txt"));
            CHECK(!fs.open("../outside_secret.txt", "w"));
            CHECK(!fs.remove("/../outside_secret.txt"));
            CHECK(stdfs::exists(root.parent_path() / "outside_secret.txt"));
            stdfs::remove(root.parent_path() / "outside_secret.txt");
            std::string hp;
            CHECK(!fs.hostPath("/a/../b", hp));
            CHECK(fs.hostPath("//a/./b//c", hp));
            CHECK(stdfs::path(hp) == root / "a" / "b" / "c");
        }

        // format empties the root
        CHECK(fs.format());
        CHECK(!fs.exists("/dir/a.txt"));

        // the global LittleFS / SPIFFS objects can be re-rooted
        LittleFS.setRoot(root.string());
        CHECK(LittleFS.begin());
        LittleFS.open("/g.txt", "w").print("g");
        CHECK(LittleFS.exists("/g.txt"));

        stdfs::remove_all(root);
    }

    // ===================== Print / Stream =====================
    {
        // a minimal Print that collects what is written
        struct Collect : public Print {
            std::string out;
            size_t write(uint8_t c) override { out.push_back(static_cast<char>(c)); return 1; }
        } p;
        CHECK(p.print("ab") == 2);
        p.print(String("cd"));
        p.print('e');
        p.print(42);
        p.print(-7);
        p.print(255, HEX);
        p.print(5u, BIN);
        p.print(3.14159, 3);
        p.print(static_cast<unsigned char>(65));  // numbers, not characters
        CHECK(p.out == "abcde42-7ff1013.14265");
        p.out.clear();
        p.println("x");
        p.println(7);
        p.println();
        CHECK(p.out == "x\r\n7\r\n\r\n");
        p.out.clear();
        CHECK(p.printf("%d-%s-%05.1f", 5, "z", 2.5) == 9);
        CHECK(p.out == "5-z-002.5");
        p.out.clear();
        std::string longFmt(500, 'k');
        p.printf("%s", longFmt.c_str());  // longer than the internal stack buffer
        CHECK(p.out == longFmt);

        // Serial is a Print/Stream
        Print& serial = Serial;
        serial.print("");
        CHECK(Serial.available() == 0 && Serial.read() == -1);

        // File is a Stream: readBytes / readStringUntil / print into it
        stdfs::path root = stdfs::temp_directory_path() / ("espasyncemu_stream_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        FS fs(root.string());
        fs.begin();
        {
            File f = fs.open("/s.txt", "w");
            Print& as = f;
            as.print("alpha,beta\ngamma");
        }
        File f = fs.open("/s.txt");
        Stream& st = f;
        CHECK(st.readStringUntil(',') == "alpha");
        char buf[8] = {0};
        CHECK(st.readBytes(buf, 4) == 4 && std::string(buf) == "beta");
        CHECK(st.peek() == '\n');
        CHECK(st.readString() == "\ngamma");
        CHECK(st.read() == -1);
        stdfs::remove_all(root);
    }

    std::puts("compat_test OK");
    return 0;
}
