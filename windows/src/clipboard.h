#pragma once
#include "common.h"

// BGRA(위→아래 행) 그림을 클립보드에 넣는다. PNG(디스코드·브라우저·포토샵이 우선 씀)와
// 비트맵(CF_DIB, 그림판 등)을 같이 넣어 어디에 붙여 넣어도 원본 화질 그대로 들어가게 한다.
bool CopyImageToClipboard(HWND owner, const std::vector<uint8_t>& bgra, int width, int height, std::wstring& error);
