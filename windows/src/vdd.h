#pragma once
#include "common.h"

// Virtual Display Driver(VirtualDrivers/Virtual-Display-Driver) 제어.
// 드라이버의 제어 파이프(\\.\pipe\MTTVirtualDisplayPipe, Everyone 허용)에 SETDISPLAYCOUNT를 보내므로
// 관리자 권한이 필요 없다. 공식 Virtual Driver Control 앱과 같은 방식.

bool VddInstalled();  // 제어 파이프가 떠 있는지 (드라이버가 동작 중인지)
bool VddSetDisplayCount(int count, std::wstring& error);  // 0이면 가상 모니터를 모두 뺀다
void VddSetDisplayCountAsync(int count);                   // 결과는 로그로

// 지금 켜져 있는 VDD 모니터의 GDI 이름 (\\.\DISPLAYn)
std::vector<std::wstring> VddMonitorDevices();
