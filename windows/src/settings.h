#pragma once
#include "common.h"

// 실행 파일 옆 PadLinkRecv.ini
struct Settings {
    int mode = 0;  // 0 = USB, 1 = Wi-Fi
    std::wstring ip = L"192.168.45.137";
    std::wstring asioDriver;     // 비어 있으면 첫 번째 ASIO 드라이버
    std::wstring discordDevice;  // 비어 있으면 사용 안 함 (기본 장치로 내보내면 내 귀에 이중으로 들림)
    int asioBufferMs = 40;
    int discordBufferMs = 80;
    int asioVolume = 100;
    int discordVolume = 100;
    int asioDspBuffer = 256;
    int rotation = 0;  // 사용자가 추가로 돌린 90도 단위 횟수

    std::wstring penMonitor;              // \\.\DISPLAY1 형식, 비어 있으면 주 모니터
    std::wstring squeezeKeys = L"SPACE";  // 클립 스튜디오: 누르는 동안 손 도구
    std::wstring doubleTapKeys = L"E,P";  // 클립 스튜디오: 지우개 ↔ 펜
    int pressureGamma = 100;
    int tilt = 1;
    int invertTilt = 0;
};

Settings LoadSettings();
void SaveSettings(const Settings& s);
