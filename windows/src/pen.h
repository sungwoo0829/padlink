#pragma once
#include "common.h"

#include <atomic>
#include <mutex>
#include <thread>

struct PenMapping {
    RECT target{};                      // 펜이 움직일 화면 영역 (물리 픽셀)
    std::wstring squeezeKeys = L"SPACE";  // 스퀴즈를 쥐고 있는 동안 누를 키
    std::wstring doubleTapKeys = L"E,P";  // 더블탭마다 차례로 누를 키 (쉼표로 구분)
    int pressureGamma = 100;            // 100 = 그대로, 크면 약한 필압이 더 약하게
    bool tilt = true;
    bool invertTilt = false;
};

struct MonitorInfo {
    std::wstring device;  // \\.\DISPLAY1
    RECT rect{};
    bool primary = false;
};
std::vector<MonitorInfo> ListMonitors();

// 마지막으로 넣은 값과, 마지막 TakeRange 이후의 범위
struct PenStats {
    bool any = false;
    double pressure = 0, maxPressure = 0;
    int tiltX = 0, tiltY = 0, rotation = 0;
    int minTiltX = 0, maxTiltX = 0, minTiltY = 0, maxTiltY = 0;
    double altitudeDeg = 90;
};

// iPad Pencil 샘플 → Windows 합성 펜(InjectSyntheticPointerInput), 버튼 → 키보드
class PenInjector {
public:
    ~PenInjector();
    bool Init(std::wstring& error);
    void Configure(const PenMapping& mapping);
    RECT Target();
    void OnSamples(const uint8_t* data, size_t len);
    void OnButton(uint8_t button, uint8_t phase);
    void Reset();  // 연결이 끊기면 펜을 떼고 눌린 키를 놓는다
    PenStats Live();
    PenStats TakeRange();

    std::atomic<uint64_t> samples{0};
    std::atomic<uint64_t> buttons{0};

private:
    enum class State { Out, Hover, Contact };
    struct Sample {
        double x = 0, y = 0, pressure = 0, altitude = 0, azimuth = 0, roll = 0;
    };

    void Keepalive();
    void HandleLocked(uint8_t phase, const Sample& s);
    bool InjectLocked(POINTER_FLAGS flags, const Sample& s);
    void ReleaseKeysLocked();

    HSYNTHETICPOINTERDEVICE device_ = nullptr;
    std::mutex mutex_;
    PenMapping mapping_;
    State state_ = State::Out;
    Sample last_;
    ULONGLONG lastInject_ = 0;
    ULONGLONG lastData_ = 0;
    bool squeezeHeld_ = false;
    std::wstring heldKeys_;
    size_t doubleTapIndex_ = 0;
    bool warned_ = false;
    PenStats live_;
    PenStats range_;

    std::thread thread_;
    std::atomic<bool> stop_{false};
};
