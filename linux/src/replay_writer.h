#pragma once
#include <string>

#include "replay.h"

// 하이라이트를 MP4로 쓴다: 영상은 받은 H.264 그대로(다시 인코딩 안 함), 소리는 AAC.
// turns: 똑바로 보려면 시계 방향으로 90도를 몇 번 돌리는지 (파일에 회전 정보로 남긴다)
bool WriteReplayMp4(const ReplayClip& clip, int turns, int width, int height, const std::string& path,
                    std::string& error);

// ~/비디오/PadLink 같은 저장 폴더 (XDG 비디오 폴더, 없으면 만든다)
std::string ReplayFolder();
