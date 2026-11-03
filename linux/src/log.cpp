#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
std::deque<std::string> g_lines;
uint64_t g_version = 0;
constexpr size_t kMaxLines = 500;
}  // namespace

void LogInit(const std::string& path) {
    std::lock_guard lock(g_mutex);
    g_file = std::fopen(path.c_str(), "w");
}

void Log(const std::string& line) {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm local{};
    localtime_r(&ts.tv_sec, &local);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03ld ", local.tm_hour, local.tm_min, local.tm_sec,
                  ts.tv_nsec / 1000000);
    std::string text = stamp + line;
    std::lock_guard lock(g_mutex);
    if (g_file) {
        std::fprintf(g_file, "%s\n", text.c_str());
        std::fflush(g_file);
    }
    g_lines.push_back(std::move(text));
    if (g_lines.size() > kMaxLines) g_lines.pop_front();
    ++g_version;
}

std::vector<std::string> LogLines() {
    std::lock_guard lock(g_mutex);
    return {g_lines.begin(), g_lines.end()};
}

uint64_t LogVersion() {
    std::lock_guard lock(g_mutex);
    return g_version;
}

std::string Format(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}
