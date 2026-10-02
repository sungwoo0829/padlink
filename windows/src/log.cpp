#include "log.h"

#include <cstdio>
#include <mutex>

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
HWND g_hwnd = nullptr;
UINT g_message = 0;
}  // namespace

void LogInit() {
    std::wstring path = ExeDir() + L"\\PadLinkRecv.log";
    _wfopen_s(&g_file, path.c_str(), L"w, ccs=UTF-8");
}

void LogSetWindow(HWND hwnd, UINT message) {
    std::lock_guard lock(g_mutex);
    g_hwnd = hwnd;
    g_message = message;
}

void Log(const std::wstring& line) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::wstring text = Format(L"%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds) + line;
    std::lock_guard lock(g_mutex);
    if (g_file) {
        fwprintf(g_file, L"%ls\n", text.c_str());
        fflush(g_file);
    }
    if (g_hwnd) {
        auto* copy = new std::wstring(std::move(text));
        if (!PostMessageW(g_hwnd, g_message, 0, (LPARAM)copy)) delete copy;
    }
}
