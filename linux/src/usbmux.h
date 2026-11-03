#pragma once
#include <cstdint>
#include <string>
#include <vector>

// usbmuxd(/var/run/usbmuxd, 우분투 usbmuxd 패키지)를 통해 USB로 연결된 iPad의 TCP 포트에 붙는다.
// 프로토콜은 Windows의 Apple Mobile Device Service와 같다.
struct UsbmuxDevice {
    int id = 0;
    std::string serial;
    std::string connectionType;  // "USB" 또는 "Network"
};

bool UsbmuxList(std::vector<UsbmuxDevice>& out, std::string& error);

// 성공하면 기기 포트로 이어진 소켓. 실패하면 -1, result = usbmux 결과 코드(3 = 기기 쪽 포트 닫힘)
int UsbmuxConnect(int deviceId, uint16_t port, int& result, std::string& error);

bool SendAll(int fd, const void* data, size_t len);
bool RecvAll(int fd, void* data, size_t len);
