#pragma once
#include "common.h"

// Apple Mobile Device Service(Apple Devices 앱/iTunes에 포함)의 usbmuxd(127.0.0.1:27015)를 통해
// USB로 연결된 iPad의 TCP 포트에 직접 붙는다.
struct UsbmuxDevice {
    int id = 0;
    std::string serial;
    std::string connectionType;  // "USB" 또는 "Network"
};

bool UsbmuxList(std::vector<UsbmuxDevice>& out, std::wstring& error);

// 성공하면 기기 포트로 이어진 소켓. 실패 시 INVALID_SOCKET, result = usbmux 결과 코드(3 = 기기 쪽 포트 닫힘)
SOCKET UsbmuxConnect(int deviceId, uint16_t port, int& result, std::wstring& error);
