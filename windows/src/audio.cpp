#include "audio.h"

#include <mmsystem.h>
#include <objbase.h>

#include <algorithm>
#include <cstring>
#include <type_traits>

#include "log.h"

namespace {
// FMOD Core C API에서 쓰는 것만 (fmod_common.h 기준)
struct FMOD_SYSTEM;
struct FMOD_SOUND;
struct FMOD_CHANNEL;
struct FMOD_CHANNELGROUP;
struct FMOD_CREATESOUNDEXINFO;
using FRESULT = int;
constexpr int kOutputWasapi = 6;  // FMOD_OUTPUTTYPE_WASAPI
constexpr int kOutputAsio = 7;    // FMOD_OUTPUTTYPE_ASIO
constexpr unsigned kModeLoopNormal = 0x00000002;
constexpr unsigned kMode2D = 0x00000008;
constexpr unsigned kModeCreateSample = 0x00000100;
constexpr unsigned kTimeUnitPcm = 0x00000002;

// FMOD에 넘길 1초짜리 무음 WAV. 이 소리를 무한 반복 재생하면서 재생 위치 앞쪽에 받은 소리를 써 넣는다.
std::wstring SilenceWav(int rate) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring path = std::wstring(dir) + L"padlink_silence_" + std::to_wstring(rate) + L".wav";
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;

    uint32_t dataBytes = uint32_t(rate) * 4;
    std::vector<uint8_t> file(44 + dataBytes, 0);
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&file[at], &v, 2); };
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&file[at], &v, 4); };
    std::memcpy(&file[0], "RIFF", 4);
    put32(4, 36 + dataBytes);
    std::memcpy(&file[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 2);
    put32(24, uint32_t(rate));
    put32(28, uint32_t(rate) * 4);
    put16(32, 4);
    put16(34, 16);
    std::memcpy(&file[36], "data", 4);
    put32(40, dataBytes);

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, file.data(), (DWORD)file.size(), &written, nullptr);
        CloseHandle(h);
    }
    return path;
}
}  // namespace

struct AudioEngine::Api {
    HMODULE dll = nullptr;
    unsigned headerVersion = 0;
    FRESULT(__stdcall* System_Create)(FMOD_SYSTEM**, unsigned) = nullptr;
    FRESULT(__stdcall* System_Release)(FMOD_SYSTEM*) = nullptr;
    FRESULT(__stdcall* System_SetOutput)(FMOD_SYSTEM*, int) = nullptr;
    FRESULT(__stdcall* System_GetNumDrivers)(FMOD_SYSTEM*, int*) = nullptr;
    FRESULT(__stdcall* System_GetDriverInfo)(FMOD_SYSTEM*, int, char*, int, void*, int*, int*, int*) = nullptr;
    FRESULT(__stdcall* System_SetDriver)(FMOD_SYSTEM*, int) = nullptr;
    FRESULT(__stdcall* System_SetSoftwareFormat)(FMOD_SYSTEM*, int, int, int) = nullptr;
    FRESULT(__stdcall* System_SetDSPBufferSize)(FMOD_SYSTEM*, unsigned, int) = nullptr;
    FRESULT(__stdcall* System_Init)(FMOD_SYSTEM*, int, unsigned, void*) = nullptr;
    FRESULT(__stdcall* System_Update)(FMOD_SYSTEM*) = nullptr;
    FRESULT(__stdcall* System_CreateSound)(FMOD_SYSTEM*, const char*, unsigned, FMOD_CREATESOUNDEXINFO*,
                                           FMOD_SOUND**) = nullptr;
    FRESULT(__stdcall* System_PlaySound)(FMOD_SYSTEM*, FMOD_SOUND*, FMOD_CHANNELGROUP*, int, FMOD_CHANNEL**) = nullptr;
    FRESULT(__stdcall* Sound_Release)(FMOD_SOUND*) = nullptr;
    FRESULT(__stdcall* Sound_Lock)(FMOD_SOUND*, unsigned, unsigned, void**, void**, unsigned*, unsigned*) = nullptr;
    FRESULT(__stdcall* Sound_Unlock)(FMOD_SOUND*, void*, void*, unsigned, unsigned) = nullptr;
    FRESULT(__stdcall* Channel_GetPosition)(FMOD_CHANNEL*, unsigned*, unsigned) = nullptr;
    FRESULT(__stdcall* Channel_SetFrequency)(FMOD_CHANNEL*, float) = nullptr;
    FRESULT(__stdcall* Channel_SetVolume)(FMOD_CHANNEL*, float) = nullptr;
    FRESULT(__stdcall* Channel_SetPaused)(FMOD_CHANNEL*, int) = nullptr;
    FRESULT(__stdcall* Channel_Stop)(FMOD_CHANNEL*) = nullptr;

    ~Api() {
        if (dll) FreeLibrary(dll);
    }
};

struct AudioEngine::Output {
    explicit Output(bool isAsio) : asio(isAsio) {}
    const bool asio;
    FMOD_SYSTEM* sys = nullptr;
    FMOD_SOUND* sound = nullptr;
    FMOD_CHANNEL* channel = nullptr;
    std::wstring device;
    std::wstring error;
    int deviceRate = 0;
    int bufferMs = 40;
    float volume = 1.0f;

    int soundRate = 0;
    unsigned soundLen = 0;
    unsigned writePos = 0;

    uint64_t readPos = 0;
    uint64_t generation = UINT64_MAX;
    bool buffering = true;
    double fillSmooth = 0;
    double ratio = 1.0;
    uint32_t drained = 0;
    uint32_t late = 0;
    uint32_t skips = 0;
    std::vector<int16_t> scratch;
};

AudioEngine::AudioEngine() : asio_(std::make_unique<Output>(true)), discord_(std::make_unique<Output>(false)) {}

AudioEngine::~AudioEngine() { Stop(); }

bool AudioEngine::Load(std::wstring& error) {
    auto api = std::make_unique<Api>();
    std::wstring local = ExeDir() + L"\\fmod.dll";
    api->dll = LoadLibraryW(local.c_str());
    if (!api->dll) api->dll = LoadLibraryW(L"fmod.dll");
    if (!api->dll) {
        error = L"fmod.dll 없음 — FMOD Engine(Windows) SDK의 api\\core\\lib\\x64\\fmod.dll을 PadLinkRecv.exe 옆에 두세요. "
                L"그동안 소리는 꺼진 채로 동작합니다.";
        return false;
    }
    bool ok = true;
    auto load = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(
            reinterpret_cast<void*>(GetProcAddress(api->dll, name)));
        if (!fn && ok) {
            ok = false;
            error = L"fmod.dll에 " + Widen(name) + L" 함수가 없음";
        }
    };
    load(api->System_Create, "FMOD_System_Create");
    load(api->System_Release, "FMOD_System_Release");
    load(api->System_SetOutput, "FMOD_System_SetOutput");
    load(api->System_GetNumDrivers, "FMOD_System_GetNumDrivers");
    load(api->System_GetDriverInfo, "FMOD_System_GetDriverInfo");
    load(api->System_SetDriver, "FMOD_System_SetDriver");
    load(api->System_SetSoftwareFormat, "FMOD_System_SetSoftwareFormat");
    load(api->System_SetDSPBufferSize, "FMOD_System_SetDSPBufferSize");
    load(api->System_Init, "FMOD_System_Init");
    load(api->System_Update, "FMOD_System_Update");
    load(api->System_CreateSound, "FMOD_System_CreateSound");
    load(api->System_PlaySound, "FMOD_System_PlaySound");
    load(api->Sound_Release, "FMOD_Sound_Release");
    load(api->Sound_Lock, "FMOD_Sound_Lock");
    load(api->Sound_Unlock, "FMOD_Sound_Unlock");
    load(api->Channel_GetPosition, "FMOD_Channel_GetPosition");
    load(api->Channel_SetFrequency, "FMOD_Channel_SetFrequency");
    load(api->Channel_SetVolume, "FMOD_Channel_SetVolume");
    load(api->Channel_SetPaused, "FMOD_Channel_SetPaused");
    load(api->Channel_Stop, "FMOD_Channel_Stop");
    if (!ok) return false;

    // FMOD_System_Create는 헤더 버전이 DLL과 맞아야 한다. 헤더 없이 쓰므로 2.xx.yy를 차례로 시도한다.
    for (unsigned major = 0; major < 0x10 && !api->headerVersion; ++major) {
        for (unsigned minor = 0; minor < 0x40; ++minor) {
            unsigned version = 0x00020000u | (major << 8) | minor;
            FMOD_SYSTEM* probe = nullptr;
            if (api->System_Create(&probe, version) == 0 && probe) {
                api->System_Release(probe);
                api->headerVersion = version;
                break;
            }
        }
    }
    if (!api->headerVersion) {
        error = L"이 fmod.dll 버전을 인식할 수 없음 (FMOD 2.x 64비트가 필요)";
        return false;
    }
    Log(Format(L"FMOD %x.%02x.%02x 불러옴", api->headerVersion >> 16, (api->headerVersion >> 8) & 0xFF,
               api->headerVersion & 0xFF));
    api_ = std::move(api);
    return true;
}

std::vector<std::wstring> AudioEngine::Drivers(bool asio) {
    std::vector<std::wstring> names;
    if (!api_) return names;
    FMOD_SYSTEM* sys = nullptr;
    if (api_->System_Create(&sys, api_->headerVersion) != 0 || !sys) return names;
    if (api_->System_SetOutput(sys, asio ? kOutputAsio : kOutputWasapi) == 0) {
        int count = 0;
        api_->System_GetNumDrivers(sys, &count);
        for (int i = 0; i < count; ++i) {
            char name[256] = {};
            if (api_->System_GetDriverInfo(sys, i, name, sizeof(name), nullptr, nullptr, nullptr, nullptr) == 0)
                names.push_back(Widen(name));
        }
    }
    api_->System_Release(sys);
    return names;
}

void AudioEngine::Start() {
    if (!api_ || thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { Thread(); });
}

void AudioEngine::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void AudioEngine::Apply(const AudioConfig& config) {
    std::lock_guard lock(configMutex_);
    config_ = config;
    configDirty_ = true;
}

void AudioEngine::Push(const int16_t* pcm, size_t frames, int sampleRate) {
    if (sampleRate <= 0 || frames == 0) return;
    std::lock_guard lock(ringMutex_);
    if (sampleRate != ringRate_) {
        ringRate_ = sampleRate;
        ringFrames_ = size_t(sampleRate) * 2;
        ring_.assign(ringFrames_ * 2, 0);
        ringWrite_ = 0;
        ++ringGeneration_;
        Log(Format(L"iPad 소리 %dHz", sampleRate));
    }
    for (size_t i = 0; i < frames; ++i) {
        size_t at = size_t((ringWrite_ + i) % ringFrames_) * 2;
        ring_[at] = pcm[i * 2];
        ring_[at + 1] = pcm[i * 2 + 1];
    }
    ringWrite_ += frames;
}

AudioStats AudioEngine::Stats() {
    std::lock_guard lock(statsMutex_);
    return stats_;
}

void AudioEngine::Thread() {
    // ASIO 드라이버는 대부분 STA COM 객체라 이 스레드를 STA로 둔다
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    timeBeginPeriod(1);
    while (!stop_) {
        AudioConfig config;
        bool apply = false;
        {
            std::lock_guard lock(configMutex_);
            if (configDirty_) {
                config = config_;
                configDirty_ = false;
                apply = true;
            }
        }
        if (apply) {
            OpenOutput(*asio_, config.asioDriver, config.asioBufferMs, config.asioVolume, config.asioDspBuffer);
            OpenOutput(*discord_, config.discordDevice, config.discordBufferMs, config.discordVolume, 0);
        }

        int rate = 0;
        {
            std::lock_guard lock(ringMutex_);
            rate = ringRate_;
        }
        for (Output* o : {asio_.get(), discord_.get()}) {
            if (o->sys && rate > 0) EnsureSound(*o, rate);
            Pump(*o);
        }
        {
            std::lock_guard lock(statsMutex_);
            stats_.sourceRate = rate;
            Snapshot(*asio_, stats_.asio);
            Snapshot(*discord_, stats_.discord);
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(2);
    }
    CloseOutput(*asio_);
    CloseOutput(*discord_);
    timeEndPeriod(1);
    CoUninitialize();
}

void AudioEngine::Snapshot(const Output& o, AudioOutputStats& s) {
    s.active = o.sys != nullptr;
    s.device = o.device;
    s.error = o.error;
    s.deviceRate = o.deviceRate;
    s.fillMs = o.soundRate > 0 ? float(o.fillSmooth * 1000.0 / o.soundRate) : 0.0f;
    s.correctionPct = float((o.ratio - 1.0) * 100.0);
    s.drained = o.drained;
    s.late = o.late;
    s.skips = o.skips;
}

void AudioEngine::CloseOutput(Output& o) {
    if (o.channel) api_->Channel_Stop(o.channel);
    if (o.sound) api_->Sound_Release(o.sound);
    if (o.sys) api_->System_Release(o.sys);
    o.channel = nullptr;
    o.sound = nullptr;
    o.sys = nullptr;
    o.soundRate = 0;
    o.soundLen = 0;
    o.writePos = 0;
    o.generation = UINT64_MAX;
    o.ratio = 1.0;
    o.fillSmooth = 0;
}

void AudioEngine::OpenOutput(Output& o, const std::wstring& wanted, int bufferMs, int volume, int dspBuffer) {
    CloseOutput(o);
    const wchar_t* label = o.asio ? L"ASIO" : L"디스코드용";
    o.bufferMs = std::clamp(bufferMs, 10, 500);
    o.volume = std::clamp(volume, 0, 200) / 100.0f;
    o.error.clear();
    o.device.clear();
    o.deviceRate = 0;
    o.drained = 0;
    o.late = 0;
    o.skips = 0;
    if (!o.asio && wanted.empty()) {
        o.error = L"장치를 고르지 않음";
        return;
    }
    const int type = o.asio ? kOutputAsio : kOutputWasapi;

    // 드라이버 번호와 장치 샘플레이트 찾기
    int index = -1, deviceRate = 0, count = 0;
    {
        FMOD_SYSTEM* sys = nullptr;
        if (api_->System_Create(&sys, api_->headerVersion) != 0 || !sys) {
            o.error = L"FMOD 시스템 생성 실패";
            return;
        }
        if (api_->System_SetOutput(sys, type) != 0) {
            api_->System_Release(sys);
            o.error = o.asio ? L"이 fmod.dll에서 ASIO를 쓸 수 없음" : L"WASAPI 출력 설정 실패";
            return;
        }
        api_->System_GetNumDrivers(sys, &count);
        for (int i = 0; i < count; ++i) {
            char name[256] = {};
            int r = 0;
            if (api_->System_GetDriverInfo(sys, i, name, sizeof(name), nullptr, &r, nullptr, nullptr) != 0) continue;
            std::wstring n = Widen(name);
            if (wanted.empty() || n == wanted) {
                index = i;
                deviceRate = r;
                o.device = n;
                break;
            }
        }
        api_->System_Release(sys);
    }
    if (index < 0) {
        o.error = count == 0 ? (o.asio ? L"ASIO 드라이버 없음" : L"출력 장치 없음") : L"장치를 찾을 수 없음: " + wanted;
        Log(std::wstring(label) + L": " + o.error);
        return;
    }

    FRESULT lastError = 0;
    auto build = [&](bool customDsp) -> FMOD_SYSTEM* {
        FMOD_SYSTEM* sys = nullptr;
        if (api_->System_Create(&sys, api_->headerVersion) != 0 || !sys) return nullptr;
        api_->System_SetOutput(sys, type);
        api_->System_SetDriver(sys, index);
        api_->System_SetSoftwareFormat(sys, deviceRate > 0 ? deviceRate : 48000, 0, 0);
        if (customDsp) api_->System_SetDSPBufferSize(sys, (unsigned)dspBuffer, 4);
        lastError = api_->System_Init(sys, 16, 0, nullptr);
        if (lastError != 0) {
            api_->System_Release(sys);
            return nullptr;
        }
        return sys;
    };
    o.sys = build(o.asio && dspBuffer > 0);
    if (!o.sys && o.asio && dspBuffer > 0) {
        Log(Format(L"ASIO: DSP 버퍼 %d로 시작 실패(FMOD 오류 %d), 기본값으로 다시 시도", dspBuffer, lastError));
        o.sys = build(false);
    }
    if (!o.sys) {
        o.error = Format(L"시작 실패 (FMOD 오류 %d) — 다른 프로그램이 장치를 쓰고 있는지 확인", lastError);
        Log(std::wstring(label) + L" " + o.device + L": " + o.error);
        return;
    }
    o.deviceRate = deviceRate;
    Log(Format(L"%ls 출력: %ls (%dHz, 버퍼 %dms)", label, o.device.c_str(), deviceRate, o.bufferMs));
}

bool AudioEngine::EnsureSound(Output& o, int rate) {
    if (o.sound && o.soundRate == rate) return true;
    if (o.channel) api_->Channel_Stop(o.channel);
    if (o.sound) api_->Sound_Release(o.sound);
    o.channel = nullptr;
    o.sound = nullptr;

    std::string path = Narrow(SilenceWav(rate));
    if (api_->System_CreateSound(o.sys, path.c_str(), kModeLoopNormal | kMode2D | kModeCreateSample, nullptr,
                                 &o.sound) != 0 ||
        !o.sound) {
        o.sound = nullptr;
        o.error = L"재생 버퍼 생성 실패";
        return false;
    }
    if (api_->System_PlaySound(o.sys, o.sound, nullptr, 1, &o.channel) != 0 || !o.channel) {
        api_->Sound_Release(o.sound);
        o.sound = nullptr;
        o.channel = nullptr;
        o.error = L"재생 시작 실패";
        return false;
    }
    api_->Channel_SetVolume(o.channel, o.volume);
    api_->Channel_SetPaused(o.channel, 0);
    o.soundRate = rate;
    o.soundLen = unsigned(rate);
    o.writePos = 0;
    o.generation = UINT64_MAX;
    o.ratio = 1.0;
    o.fillSmooth = 0;
    return true;
}

void AudioEngine::Pump(Output& o) {
    if (!o.sys) return;
    if (o.sound && o.channel) {
        unsigned play = 0;
        if (api_->Channel_GetPosition(o.channel, &play, kTimeUnitPcm) == 0) {
            const unsigned len = o.soundLen;
            play %= len;
            unsigned ahead = (o.writePos + len - play) % len;
            if (ahead > len / 2) {
                // 쓰기가 재생 위치에 따라잡힘 (스레드가 늦게 깨어남)
                o.writePos = play;
                ahead = 0;
                ++o.late;
            }
            // 재생 위치보다 이만큼 앞까지 채워 둔다. ASIO는 지연을 줄이려고 짧게.
            unsigned target = std::max(256u, unsigned(uint64_t(o.asio ? 15 : 40) * unsigned(o.soundRate) / 1000));
            if (ahead < target) {
                unsigned n = target - ahead;
                o.scratch.resize(size_t(n) * 2);
                ReadRing(o, o.scratch.data(), n);
                void* p1 = nullptr;
                void* p2 = nullptr;
                unsigned l1 = 0, l2 = 0;
                if (api_->Sound_Lock(o.sound, o.writePos * 4, n * 4, &p1, &p2, &l1, &l2) == 0) {
                    const char* src = reinterpret_cast<const char*>(o.scratch.data());
                    if (p1 && l1) std::memcpy(p1, src, l1);
                    if (p2 && l2) std::memcpy(p2, src + l1, l2);
                    api_->Sound_Unlock(o.sound, p1, p2, l1, l2);
                }
                o.writePos = (o.writePos + n) % len;
            }
            api_->Channel_SetFrequency(o.channel, float(o.soundRate * o.ratio));
        }
    }
    api_->System_Update(o.sys);
}

void AudioEngine::ReadRing(Output& o, int16_t* dst, unsigned frames) {
    std::lock_guard lock(ringMutex_);
    if (o.generation != ringGeneration_) {
        o.generation = ringGeneration_;
        o.readPos = ringWrite_;
        o.buffering = true;
        o.fillSmooth = 0;
    }
    size_t got = 0;
    if (ringRate_ > 0) {
        const uint64_t rate = uint64_t(ringRate_);
        const uint64_t target = rate * uint64_t(o.bufferMs) / 1000;
        uint64_t fill = ringWrite_ - o.readPos;
        if (fill > ringFrames_ || fill > target + rate * 3 / 10) {
            // 너무 밀렸으면 목표 지연으로 건너뛴다
            o.readPos = ringWrite_ - std::min<uint64_t>(target, ringWrite_);
            fill = ringWrite_ - o.readPos;
            ++o.skips;
        }
        if (o.buffering && fill >= target) o.buffering = false;
        if (!o.buffering) {
            got = size_t(std::min<uint64_t>(frames, fill));
            for (size_t i = 0; i < got; ++i) {
                size_t at = size_t((o.readPos + i) % ringFrames_) * 2;
                dst[i * 2] = ring_[at];
                dst[i * 2 + 1] = ring_[at + 1];
            }
            o.readPos += got;
            if (got < frames) {
                o.buffering = true;
                ++o.drained;
            }
        }
        // 클럭 차이 보정: 버퍼가 목표보다 차 있으면 살짝 빠르게, 모자라면 살짝 느리게 (최대 ±0.5%)
        double now = double(ringWrite_ - o.readPos);
        o.fillSmooth += 0.02 * (now - o.fillSmooth);
        double errorSeconds = (o.fillSmooth - double(target)) / double(rate);
        o.ratio = 1.0 + std::clamp(errorSeconds * 0.05, -0.005, 0.005);
    }
    std::fill(dst + got * 2, dst + size_t(frames) * 2, int16_t(0));
}
