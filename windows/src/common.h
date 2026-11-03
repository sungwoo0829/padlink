#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "wire.h"

std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);
std::wstring ExeDir();
std::wstring Format(const wchar_t* fmt, ...);
bool SendAll(SOCKET s, const void* data, size_t len);
bool RecvAll(SOCKET s, void* data, size_t len);
