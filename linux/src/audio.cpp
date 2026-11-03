#include "audio.h"

#include <algorithm>

#include "log.h"

std::vector<std::string> AudioOut::Devices() {
    std::vector<std::string> names;
    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
    for (int i = 0; ids && i < count; ++i) {
        if (const char* name = SDL_GetAudioDeviceName(ids[i])) names.emplace_back(name);
    }
    SDL_free(ids);
    return names;
}

void AudioOut::Open(const std::string& device, int bufferMs, int volume) {
    Close();
    SDL_AudioDeviceID id = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    std::string chosen = "기본 출력";
    if (!device.empty()) {
        int count = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
        bool found = false;
        for (int i = 0; ids && i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            if (name && device == name) {
                id = ids[i];
                chosen = name;
                found = true;
                break;
            }
        }
        SDL_free(ids);
        if (!found) Log("소리: 출력 장치를 찾을 수 없어 기본 출력으로: " + device);
    }
    // 오인페 모니터링용이라 장치 버퍼를 짧게 (256프레임 ≈ 5ms)
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
    SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, 48000};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(id, &spec, nullptr, nullptr);
    if (!stream) {
        Log(std::string("소리: 출력을 열 수 없음 — ") + SDL_GetError());
        return;
    }
    SDL_SetAudioStreamGain(stream, std::clamp(volume, 0, 200) / 100.0f);
    std::lock_guard lock(mutex_);
    stream_ = stream;  // 장치는 멈춘 채로 열린다. 버퍼가 차면 재생한다.
    device_ = chosen;
    sourceRate_ = 0;
    bufferMs_ = std::clamp(bufferMs, 10, 500);
    buffering_ = true;
    smoothMs_ = 0;
    ratio_ = 1.0f;
    drained_ = skips_ = 0;
    Log(Format("소리 출력: %s (버퍼 %dms)", chosen.c_str(), bufferMs_));
}

void AudioOut::Close() {
    std::lock_guard lock(mutex_);
    if (stream_) SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
}

void AudioOut::SetBuffer(int bufferMs) {
    std::lock_guard lock(mutex_);
    bufferMs_ = std::clamp(bufferMs, 10, 500);
}

void AudioOut::SetVolume(int volume) {
    std::lock_guard lock(mutex_);
    if (stream_) SDL_SetAudioStreamGain(stream_, std::clamp(volume, 0, 200) / 100.0f);
}

void AudioOut::Push(const int16_t* pcm, size_t frames, int sampleRate) {
    if (sampleRate <= 0 || frames == 0) return;
    std::lock_guard lock(mutex_);
    if (!stream_) return;
    if (sampleRate != sourceRate_) {
        SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, sampleRate};
        SDL_SetAudioStreamFormat(stream_, &spec, nullptr);
        SDL_ClearAudioStream(stream_);
        SDL_PauseAudioStreamDevice(stream_);
        sourceRate_ = sampleRate;
        buffering_ = true;
        Log(Format("iPad 소리 %dHz", sampleRate));
    }
    SDL_PutAudioStreamData(stream_, pcm, int(frames * 4));
}

void AudioOut::Tick() {
    std::lock_guard lock(mutex_);
    if (!stream_ || sourceRate_ <= 0) return;
    // 아직 재생되지 않고 쌓여 있는 양 (iPad 쪽 샘플 기준)
    double queuedMs = SDL_GetAudioStreamQueued(stream_) / 4.0 / sourceRate_ * 1000.0;
    if (buffering_) {
        if (queuedMs >= bufferMs_) {
            buffering_ = false;
            smoothMs_ = queuedMs;
            SDL_ResumeAudioStreamDevice(stream_);
        }
        return;
    }
    if (queuedMs <= 0.0) {
        ++drained_;
        buffering_ = true;
        SDL_PauseAudioStreamDevice(stream_);
        return;
    }
    if (queuedMs > bufferMs_ + 300) {
        // 너무 밀렸으면 비우고 다시 쌓는다
        SDL_ClearAudioStream(stream_);
        SDL_PauseAudioStreamDevice(stream_);
        ++skips_;
        buffering_ = true;
        return;
    }
    // 버퍼가 목표보다 차 있으면 살짝 빠르게, 모자라면 살짝 느리게 (최대 ±0.5%)
    smoothMs_ += 0.02 * (queuedMs - smoothMs_);
    double error = (smoothMs_ - bufferMs_) / 1000.0;
    ratio_ = float(1.0 + std::clamp(error * 0.05, -0.005, 0.005));
    SDL_SetAudioStreamFrequencyRatio(stream_, ratio_);
}

AudioStats AudioOut::Stats() {
    std::lock_guard lock(mutex_);
    AudioStats s;
    s.active = stream_ != nullptr;
    s.device = device_;
    s.sourceRate = sourceRate_;
    s.bufferedMs = float(smoothMs_);
    s.correctionPct = (ratio_ - 1.0f) * 100.0f;
    s.drained = drained_;
    s.skips = skips_;
    return s;
}
