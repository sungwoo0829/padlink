#pragma once
#include <SDL3/SDL.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct AudioStats {
    bool active = false;
    std::string device;
    int sourceRate = 0;
    float bufferedMs = 0;
    float correctionPct = 0;
    uint32_t drained = 0;  // 받은 소리가 모자라 끊김 (버퍼가 짧거나 연결이 늦음)
    uint32_t skips = 0;    // 너무 밀려서 건너뜀
};

// iPad 소리를 PipeWire(SDL3)로 고른 출력(오디오 인터페이스 등)에 낸다.
// 리눅스 디스코드는 이 앱의 소리를 PipeWire에서 그대로 잡으므로 Windows판처럼 두 갈래로 낼 필요가 없다.
// iPad와 출력 장치의 클럭 차이는 재생 속도를 ±0.5% 안에서 조절해 맞춘다.
class AudioOut {
public:
    ~AudioOut() { Close(); }
    std::vector<std::string> Devices();  // 메인 스레드
    void Open(const std::string& device, int bufferMs, int volume);  // 메인 스레드
    void Close();
    void SetBuffer(int bufferMs);
    void SetVolume(int volume);
    void Push(const int16_t* pcm, size_t frames, int sampleRate);  // 네트워크 스레드
    void Tick();  // 메인 스레드에서 자주 (클럭 보정)
    AudioStats Stats();

private:
    std::mutex mutex_;
    SDL_AudioStream* stream_ = nullptr;
    std::string device_;
    int sourceRate_ = 0;
    int bufferMs_ = 40;
    bool buffering_ = true;
    double smoothMs_ = 0;
    float ratio_ = 1.0f;
    uint32_t drained_ = 0;
    uint32_t skips_ = 0;
};
