#pragma once
#include "common.h"

// 실행 파일 옆 PadLinkRecv.log와 창의 로그 칸에 함께 쓴다 (어느 스레드에서든 호출 가능)
void LogInit();
void LogSetWindow(HWND hwnd, UINT message);  // lParam = new std::wstring*, 받는 쪽이 delete
void Log(const std::wstring& line);
