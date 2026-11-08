// Logs for what the host finds out, one file each in /storage/emulated/0/Black/SaveData/logs/ (the folder the other
// logs of this port are read from). A run starts a fresh file and keeps the run before as <name>_prev.log, and a
// file stops growing at a size: a runaway line cannot fill the storage. Only writes, changes nothing
#pragma once

#include <sys/stat.h>

#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace filelog {

constexpr const char* kDir = "/storage/emulated/0/Black/SaveData/logs/";
constexpr size_t kMaxBytes = 600 * 1024;

// one line, the newline added
__attribute__((format(printf, 2, 3))) inline void Write(const char* name, const char* fmt, ...) {
    static std::mutex lock;
    static std::map<std::string, size_t> written;  // bytes of each file this run
    std::lock_guard<std::mutex> guard(lock);
    std::string path = std::string(kDir) + name + ".log";
    auto it = written.find(name);
    if (it == written.end()) {
        ::mkdir("/storage/emulated/0/Black", 0775);
        ::mkdir("/storage/emulated/0/Black/SaveData", 0775);
        ::mkdir(kDir, 0775);
        std::rename(path.c_str(), (std::string(kDir) + name + "_prev.log").c_str());
        it = written.emplace(name, 0).first;
    }
    if (it->second >= kMaxBytes) return;
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    int n = std::vfprintf(f, fmt, args);
    va_end(args);
    std::fputc('\n', f);
    std::fclose(f);
    if (n > 0) it->second += size_t(n) + 1;
}

}  // namespace filelog
