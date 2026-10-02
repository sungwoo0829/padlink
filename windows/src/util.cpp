#include "common.h"

#include <algorithm>
#include <cstdarg>
#include <cwchar>

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, n);
    size_t pos = s.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : s.substr(0, pos);
}

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return n < 0 ? std::wstring(buf) : std::wstring(buf, (size_t)n);
}

bool SendAll(SOCKET s, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        int n = send(s, p, (int)std::min<size_t>(len, 1 << 20), 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

bool RecvAll(SOCKET s, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    while (len > 0) {
        int n = recv(s, p, (int)std::min<size_t>(len, 1 << 20), 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}
