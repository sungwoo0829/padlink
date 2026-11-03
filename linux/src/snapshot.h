#pragma once
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

// 디코딩된 프레임을 화면에 보이는 방향(시계 방향 90도 × turns)의 RGBA로 바꾼다. 원본 해상도 그대로.
bool FrameToRgba(const AVFrame* frame, int turns, bool fullRange, std::vector<uint8_t>& rgba, int& width,
                 int& height);

// RGBA를 PNG로 클립보드에 넣는다 (Wayland·X11 모두, SDL3 클립보드). 메인 스레드에서만.
bool CopyPngToClipboard(const std::vector<uint8_t>& rgba, int width, int height, std::string& error);
