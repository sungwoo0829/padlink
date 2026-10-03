#pragma once
#include "common.h"

// Virtual Display Driver(VirtualDrivers/Virtual-Display-Driver) 가상 모니터 켜기/끄기.
//
// 드라이버 파이프의 SETDISPLAYCOUNT는 설정 파일만 바꾸고 실제 모니터에 반영되지 않는 버전이
// 있어서(실측), Windows 디스플레이 구성(CCD)으로 VDD 모니터의 경로를 연결하고 끊는다.
// 설정 앱의 '이 디스플레이 연결 끊기'와 같은 동작이고 관리자 권한이 필요 없다.

struct VddState {
    bool installed = false;  // VDD 모니터가 Windows에 잡혀 있음 (꺼져 있어도)
    bool active = false;     // 바탕 화면에 붙어 있음
    std::wstring device;     // 켜져 있으면 \\.\DISPLAYn
};

VddState VddQuery();
bool VddSetActive(bool active, std::wstring& error);

// 예전 버전이 SETDISPLAYCOUNT 0을 보내서 설정 파일의 모니터 수가 0이 됐으면 1로 되돌린다.
// (그대로 두면 재부팅 뒤 VDD 모니터가 아예 생기지 않는다)
void VddRepairConfig();
