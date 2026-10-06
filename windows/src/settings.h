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
    int videoBorderless = 0;  // 'PadLink 화면' 창 테두리 없이
    int videoX = 0, videoY = 0, videoW = 0, videoH = 0;  // 그 창의 화면 영역(클라이언트) 위치·크기, w=0이면 기본

    std::wstring penMonitor;              // \\.\DISPLAY1 형식, 비어 있으면 주 모니터
    std::wstring squeezeKeys = L"SPACE";  // 클립 스튜디오: 누르는 동안 손 도구
    std::wstring doubleTapKeys = L"E,P";  // 클립 스튜디오: 지우개 ↔ 펜
    int pressureGamma = 100;
    int tilt = 1;
    int invertTilt = 0;

    int display = 0;  // 1 = 펜 모니터 화면을 iPad로 보냄 (액정타블렛). VDD 자동 모드면 항상 켜짐
    int displayBitrateMbps = 60;         // 액정타블렛 화면 (60fps 고정)
    std::wstring displayCodec = L"hevc";  // hevc 또는 h264
    int broadcastBitrateMbps = 20;       // iPad → PC 송출
    int vddCount = 1;  // 가상 모니터를 켤 때 개수
    int vddOwned = 0;  // PadLink가 켠 VDD가 아직 켜져 있음 (비정상 종료 뒤 정리용)
};

Settings LoadSettings();
void SaveSettings(const Settings& s);
