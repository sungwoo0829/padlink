#include "settings.h"

#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <map>

std::string ConfigDir() {
    std::string base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        base = xdg;
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        base = std::string(home) + "/.config";
    } else {
        base = ".";
    }
    std::string dir = base + "/padlink";
    mkdir(base.c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    return dir;
}

namespace {
std::string ConfigPath() { return ConfigDir() + "/padlink-recv.conf"; }

int ToInt(const std::map<std::string, std::string>& m, const char* key, int fallback) {
    auto it = m.find(key);
    return it == m.end() || it->second.empty() ? fallback : std::atoi(it->second.c_str());
}

std::string ToString(const std::map<std::string, std::string>& m, const char* key, const std::string& fallback) {
    auto it = m.find(key);
    return it == m.end() ? fallback : it->second;
}
}  // namespace

Settings LoadSettings() {
    std::map<std::string, std::string> m;
    std::ifstream in(ConfigPath());
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq != std::string::npos) m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    Settings s;
    s.mode = ToInt(m, "mode", s.mode);
    s.ip = ToString(m, "ip", s.ip);
    s.audioDevice = ToString(m, "audio_device", s.audioDevice);
    s.bufferMs = ToInt(m, "buffer_ms", s.bufferMs);
    s.volume = ToInt(m, "volume", s.volume);
    s.broadcastBitrateMbps = ToInt(m, "broadcast_bitrate_mbps", s.broadcastBitrateMbps);
    s.rotation = ToInt(m, "rotation", s.rotation) & 3;
    s.rangeFlip = ToInt(m, "range_flip", s.rangeFlip);
    s.borderless = ToInt(m, "borderless", s.borderless);
    s.videoW = ToInt(m, "video_w", s.videoW);
    s.videoH = ToInt(m, "video_h", s.videoH);
    s.replaySeconds = ToInt(m, "replay_seconds", s.replaySeconds);
    return s;
}

void SaveSettings(const Settings& s) {
    std::ofstream out(ConfigPath());
    out << "# PadLink 수신 설정\n"
        << "mode=" << s.mode << "\n"
        << "ip=" << s.ip << "\n"
        << "audio_device=" << s.audioDevice << "\n"
        << "buffer_ms=" << s.bufferMs << "\n"
        << "volume=" << s.volume << "\n"
        << "broadcast_bitrate_mbps=" << s.broadcastBitrateMbps << "\n"
        << "rotation=" << s.rotation << "\n"
        << "range_flip=" << s.rangeFlip << "\n"
        << "borderless=" << s.borderless << "\n"
        << "video_w=" << s.videoW << "\n"
        << "video_h=" << s.videoH << "\n"
        << "replay_seconds=" << s.replaySeconds << "\n";
}
