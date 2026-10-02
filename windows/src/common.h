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

// iPad와 주고받는 프레임 형식 (docs/protocol.md)
namespace wire {
constexpr uint16_t kPort = 47800;     // 화면 방송 확장
constexpr uint16_t kPenPort = 47810;  // 앱 펜 모드
enum Kind : uint8_t {
    kHello = 0,
    kVideoFrame = 2,
    kAudioPCM = 3,
    kLog = 4,
    kPenSamples = 16,
    kPenButton = 17,
    kPenConfig = 32,  // PC → iPad
};
#pragma pack(push, 1)
struct Header {
    uint8_t kind;
    uint8_t flags;
    uint16_t reserved;
    uint32_t length;
    uint64_t pts;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 16);
}  // namespace wire

std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);
std::wstring ExeDir();
std::wstring Format(const wchar_t* fmt, ...);
bool SendAll(SOCKET s, const void* data, size_t len);
bool RecvAll(SOCKET s, void* data, size_t len);
