#pragma once
#include "common.h"
#include "replay.h"

// 하이라이트를 MP4로 쓴다 (Media Foundation Sink Writer): 영상은 받은 H.264 그대로, 소리는 AAC.
// turns: 똑바로 보려면 시계 방향으로 90도를 몇 번 돌리는지 (파일 tkhd에 회전으로 남긴다)
// 호출한 스레드에서 COM·MFStartup이 되어 있어야 한다.
bool WriteReplayMp4(const ReplayClip& clip, int turns, UINT width, UINT height, const std::wstring& path,
                    std::wstring& error);

// 동영상\PadLink 폴더 (없으면 만든다)
std::wstring ReplayFolder();
