#pragma once
#include "common.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

struct AudioConfig {
    std::wstring asioDriver;     // 비어 있으면 첫 번째 ASIO 드라이버
    std::wstring discordDevice;  // 비어 있으면 끔
    int asioBufferMs = 40;
    int discordBufferMs = 80;
    int asioVolume = 100;
    int discordVolume = 100;
    int asioDspBuffer = 256;
};

struct AudioOutputStats {
    bool active = false;
    std::wstring device;
    std::wstring error;
    int deviceRate = 0;
    float fillMs = 0;
    float correctionPct = 0;
    uint32_t drained = 0;  // 받은 소리가 모자람 (버퍼가 짧거나 네트워크가 늦음)
    uint32_t late = 0;     // 쓰기 스레드가 늦게 깨어남
    uint32_t skips = 0;
};

struct AudioStats {
    int sourceRate = 0;
    AudioOutputStats asio;
    AudioOutputStats discord;
};

// 같은 소리를 두 갈래로 낸다.
//  - ASIO: 오디오 인터페이스로, 내가 듣는 모니터링 (Windows 오디오 엔진을 안 거쳐서 디스코드에 안 잡힘)
//  - WASAPI: 내가 안 듣는 장치로, 디스코드가 이 프로세스의 소리로 캡처
// FMOD Core는 실행할 때 fmod.dll에서 불러온다 (SDK는 라이선스상 레포에 넣을 수 없음).
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    bool Load(std::wstring& error);
    bool Loaded() const { return api_ != nullptr; }
    std::vector<std::wstring> Drivers(bool asio);
    void Start();
    void Stop();
    void Apply(const AudioConfig& config);
    void Push(const int16_t* pcm, size_t frames, int sampleRate);  // s16le 스테레오 인터리브
    AudioStats Stats();

private:
    struct Api;
    struct Output;

    void Thread();
    void OpenOutput(Output& o, const std::wstring& wanted, int bufferMs, int volume, int dspBuffer);
    void CloseOutput(Output& o);
    bool EnsureSound(Output& o, int rate);
    void Pump(Output& o);
    void ReadRing(Output& o, int16_t* dst, unsigned frames);
    void Snapshot(const Output& o, AudioOutputStats& s);

    std::unique_ptr<Api> api_;
    std::unique_ptr<Output> asio_;
    std::unique_ptr<Output> discord_;

    std::thread thread_;
    std::atomic<bool> stop_{false};

    std::mutex configMutex_;
    AudioConfig config_;
    bool configDirty_ = false;

    // 받은 소리 링버퍼. 두 출력이 각자 읽기 위치를 가진다.
    std::mutex ringMutex_;
    std::vector<int16_t> ring_;
    size_t ringFrames_ = 0;
    uint64_t ringWrite_ = 0;
    uint64_t ringGeneration_ = 0;
    int ringRate_ = 0;

    std::mutex statsMutex_;
    AudioStats stats_;
};
