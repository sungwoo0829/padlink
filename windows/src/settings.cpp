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

// GetPrivateProfileInt는 음수를 0으로 읽는다. 주 모니터 왼쪽·위의 창 좌표는 음수라 문자열로 읽는다.
int ReadSigned(const wchar_t* section, const wchar_t* key, int fallback) {
    std::wstring v = ReadString(section, key, L"");
    return v.empty() ? fallback : _wtoi(v.c_str());
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
    s.videoBorderless = ReadInt(L"video", L"borderless", s.videoBorderless);
    s.videoX = ReadSigned(L"video", L"x", s.videoX);
    s.videoY = ReadSigned(L"video", L"y", s.videoY);
    s.videoW = ReadInt(L"video", L"w", s.videoW);
    s.videoH = ReadInt(L"video", L"h", s.videoH);
    s.penMonitor = ReadString(L"pen", L"monitor", s.penMonitor);
    s.squeezeKeys = ReadString(L"pen", L"squeeze", s.squeezeKeys);
    s.doubleTapKeys = ReadString(L"pen", L"double_tap", s.doubleTapKeys);
    s.pressureGamma = ReadInt(L"pen", L"pressure_gamma", s.pressureGamma);
    s.tilt = ReadInt(L"pen", L"tilt", s.tilt);
    s.invertTilt = ReadInt(L"pen", L"invert_tilt", s.invertTilt);
    s.display = ReadInt(L"display", L"enabled", s.display);
    s.displayBitrateMbps = ReadInt(L"display", L"bitrate_mbps", s.displayBitrateMbps);
    s.displayCodec = ReadString(L"display", L"codec", s.displayCodec);
    s.broadcastBitrateMbps = ReadInt(L"broadcast", L"bitrate_mbps", s.broadcastBitrateMbps);
    s.vddCount = ReadInt(L"display", L"vdd_count", s.vddCount);
    s.vddOwned = ReadInt(L"display", L"vdd_owned", s.vddOwned);
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
    Write(L"video", L"borderless", s.videoBorderless);
    Write(L"video", L"x", s.videoX);
    Write(L"video", L"y", s.videoY);
    Write(L"video", L"w", s.videoW);
    Write(L"video", L"h", s.videoH);
    Write(L"pen", L"monitor", s.penMonitor);
    Write(L"pen", L"squeeze", s.squeezeKeys);
    Write(L"pen", L"double_tap", s.doubleTapKeys);
    Write(L"pen", L"pressure_gamma", s.pressureGamma);
    Write(L"pen", L"tilt", s.tilt);
    Write(L"pen", L"invert_tilt", s.invertTilt);
    Write(L"display", L"enabled", s.display);
    Write(L"display", L"bitrate_mbps", s.displayBitrateMbps);
    Write(L"display", L"codec", s.displayCodec);
    Write(L"broadcast", L"bitrate_mbps", s.broadcastBitrateMbps);
    Write(L"display", L"vdd_count", s.vddCount);
    Write(L"display", L"vdd_owned", s.vddOwned);
}
