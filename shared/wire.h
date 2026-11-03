#pragma once
// Windows·Linux 수신 앱이 같이 쓰는 iPad 프레임 형식 (docs/protocol.md)
#include <cstdint>

namespace wire {
constexpr uint16_t kPort = 47800;     // 화면 방송 확장
constexpr uint16_t kPenPort = 47810;  // 앱 펜 모드
enum Kind : uint8_t {
    kHello = 0,
    kVideoFrame = 2,
    kAudioPCM = 3,
    kLog = 4,
    kPenSamples = 16,
    kPenButton = 17,
    kPenConfig = 32,        // PC → iPad
    kKeyframeRequest = 33,  // iPad → PC (액정타블렛 화면)
    kControl = 48,          // JSON 제어 메시지 (PC → 방송 확장: {"bitrate"})
};
#pragma pack(push, 1)
struct Header {
    uint8_t kind;
    uint8_t flags;
    uint16_t reserved;
    uint32_t length;
    uint64_t pts;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 16);

// videoFrame flags의 CGImagePropertyOrientation(iPad) → 똑바로 보려면 시계 방향으로 90도를 몇 번 돌리는지
inline int OrientationTurns(uint8_t orientation) {
    switch (orientation) {
    case 3: return 2;  // down
    case 6: return 1;  // right
    case 8: return 3;  // left
    default: return 0;
    }
}
}  // namespace wire
