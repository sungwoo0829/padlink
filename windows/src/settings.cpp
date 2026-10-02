#include "settings.h"

namespace {
std::wstring IniPath() { return ExeDir() + L"\\PadLinkRecv.ini"; }

std::wstring ReadString(const wchar_t* section, const wchar_t* key, const std::wstring& fallback) {
    wchar_t buf[512];
    GetPrivateProfileStringW(section, key, fallback.c_str(), buf, _countof(buf), IniPath().c_str());
    return buf;
}

int ReadInt(const wchar_t* section, const wchar_t* key, int fallback) {
    return (int)GetPrivateProfileIntW(section, key, fallback, IniPath().c_str());
}

void Write(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), IniPath().c_str());
}

void Write(const wchar_t* section, const wchar_t* key, int value) { Write(section, key, std::to_wstring(value)); }
}  // namespace

Settings LoadSettings() {
    Settings s;
    s.mode = ReadInt(L"link", L"mode", s.mode);
    s.ip = ReadString(L"link", L"ip", s.ip);
    s.asioDriver = ReadString(L"audio", L"asio_driver", s.asioDriver);
    s.discordDevice = ReadString(L"audio", L"discord_device", s.discordDevice);
    s.asioBufferMs = ReadInt(L"audio", L"asio_buffer_ms", s.asioBufferMs);
    s.discordBufferMs = ReadInt(L"audio", L"discord_buffer_ms", s.discordBufferMs);
    s.asioVolume = ReadInt(L"audio", L"asio_volume", s.asioVolume);
    s.discordVolume = ReadInt(L"audio", L"discord_volume", s.discordVolume);
    s.asioDspBuffer = ReadInt(L"audio", L"asio_dsp_buffer", s.asioDspBuffer);
    s.rotation = ReadInt(L"video", L"rotation", s.rotation) & 3;
    s.penMonitor = ReadString(L"pen", L"monitor", s.penMonitor);
    s.squeezeKeys = ReadString(L"pen", L"squeeze", s.squeezeKeys);
    s.doubleTapKeys = ReadString(L"pen", L"double_tap", s.doubleTapKeys);
    s.pressureGamma = ReadInt(L"pen", L"pressure_gamma", s.pressureGamma);
    s.tilt = ReadInt(L"pen", L"tilt", s.tilt);
    s.invertTilt = ReadInt(L"pen", L"invert_tilt", s.invertTilt);
    return s;
}

void SaveSettings(const Settings& s) {
    Write(L"link", L"mode", s.mode);
    Write(L"link", L"ip", s.ip);
    Write(L"audio", L"asio_driver", s.asioDriver);
    Write(L"audio", L"discord_device", s.discordDevice);
    Write(L"audio", L"asio_buffer_ms", s.asioBufferMs);
    Write(L"audio", L"discord_buffer_ms", s.discordBufferMs);
    Write(L"audio", L"asio_volume", s.asioVolume);
    Write(L"audio", L"discord_volume", s.discordVolume);
    Write(L"audio", L"asio_dsp_buffer", s.asioDspBuffer);
    Write(L"video", L"rotation", s.rotation);
    Write(L"pen", L"monitor", s.penMonitor);
    Write(L"pen", L"squeeze", s.squeezeKeys);
    Write(L"pen", L"double_tap", s.doubleTapKeys);
    Write(L"pen", L"pressure_gamma", s.pressureGamma);
    Write(L"pen", L"tilt", s.tilt);
    Write(L"pen", L"invert_tilt", s.invertTilt);
}
