#pragma once
#include <string>

// ~/.config/padlink/padlink-recv.conf (key=value)
struct Settings {
    int mode = 0;  // 0 = USB, 1 = Wi-Fi
    std::string ip = "192.168.45.137";
    std::string audioDevice;  // 비어 있으면 기본 출력
    int bufferMs = 40;        // 받은 소리를 이만큼 쌓아 두고 재생 (끊기면 늘린다)
    int volume = 100;
    int broadcastBitrateMbps = 20;  // iPad 송출 화질
    int rotation = 0;               // 사용자가 추가로 돌린 90도 횟수
    int rangeFlip = 0;              // 색 범위 수동 전환
    int borderless = 0;             // 'PadLink 화면' 창 테두리 없이
    int videoW = 0, videoH = 0;     // 그 창 크기 (0이면 기본)
};

std::string ConfigDir();  // 없으면 만든다
Settings LoadSettings();
void SaveSettings(const Settings& s);
