#pragma once
#include "common.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

struct ScreenConfig {
    std::wstring device;  // \\.\DISPLAYn — 펜이 움직이는 모니터와 같다
    int bitrateMbps = 60;
    bool hevc = true;
};

// 액정타블렛 모드: 모니터를 Desktop Duplication으로 캡처 → BT.709 NV12 → NVENC(HEVC/H.264) → iPad.
// 화면이 멈춰 있어도 60fps로 일정하게 보낸다 (바뀐 게 없으면 마지막 화면을 다시 인코딩).
class ScreenSender {
public:
    using FrameCallback = std::function<void(const uint8_t* data, size_t size, bool key)>;

    ~ScreenSender() { Stop(); }
    void Start(const ScreenConfig& config, FrameCallback onFrame);
    void Stop();
    bool Running() const { return thread_.joinable(); }
    void RequestKeyframe() { keyRequested_ = true; }
    std::wstring Error();

    std::atomic<int> width{0};
    std::atomic<int> height{0};
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> bytes{0};

private:
    void Thread();
    void SetError(const std::wstring& e);
    bool Wait(int ms);  // 멈추라는 신호가 오면 true

    ScreenConfig config_;
    FrameCallback onFrame_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> keyRequested_{true};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::wstring error_;
};
