#include "screen.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <nvEncodeAPI.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT kNvidiaVendor = 0x10DE;

// nvEncodeAPI64.dll은 NVIDIA 드라이버에 들어 있다. 한 번만 불러 둔다.
struct NvencLibrary {
    HMODULE dll = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api{};
    bool ok = false;
    std::wstring error;

    NvencLibrary() {
        dll = LoadLibraryW(L"nvEncodeAPI64.dll");
        if (!dll) {
            error = L"nvEncodeAPI64.dll 없음 — NVIDIA 드라이버가 필요";
            return;
        }
        using GetMaxVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);
        using CreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
        auto getMax = reinterpret_cast<GetMaxVersion>(
            reinterpret_cast<void*>(GetProcAddress(dll, "NvEncodeAPIGetMaxSupportedVersion")));
        auto create = reinterpret_cast<CreateInstance>(
            reinterpret_cast<void*>(GetProcAddress(dll, "NvEncodeAPICreateInstance")));
        if (!getMax || !create) {
            error = L"nvEncodeAPI64.dll에서 함수를 찾을 수 없음";
            return;
        }
        uint32_t maxVersion = 0;
        getMax(&maxVersion);
        uint32_t needed = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
        if (needed > maxVersion) {
            error = Format(L"NVIDIA 드라이버가 오래됨 (NVENC API %u.%u 필요, 드라이버는 %u.%u)", NVENCAPI_MAJOR_VERSION,
                           NVENCAPI_MINOR_VERSION, maxVersion >> 4, maxVersion & 0xF);
            return;
        }
        api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        NVENCSTATUS st = create(&api);
        if (st != NV_ENC_SUCCESS) {
            error = Format(L"NVENC 초기화 실패 (%d)", (int)st);
            return;
        }
        ok = true;
    }
};

NvencLibrary& Nvenc() {
    static NvencLibrary lib;
    return lib;
}

// 캡처 스레드 하나가 쥐고 쓰는 자원 묶음
struct Capture {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<IDXGIOutput1> output;
    ComPtr<IDXGIOutputDuplication> dup;
    UINT width = 0, height = 0;    // 모니터 크기
    UINT encW = 0, encH = 0;       // 인코딩 크기 (NV12라 짝수)

    ComPtr<ID3D11Texture2D> bgra;  // 캡처 복사본
    ComPtr<ID3D11Texture2D> nv12;  // 인코더 입력
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
    ComPtr<ID3D11VideoProcessor> vp;
    ComPtr<ID3D11VideoProcessorInputView> vpIn;
    ComPtr<ID3D11VideoProcessorOutputView> vpOut;

    void* encoder = nullptr;
    NV_ENC_REGISTERED_PTR registered = nullptr;
    NV_ENC_OUTPUT_PTR bitstream = nullptr;
    std::vector<uint8_t> out;

    ~Capture() { Close(); }

    void Close() {
        auto& nv = Nvenc().api;
        if (encoder) {
            if (registered) nv.nvEncUnregisterResource(encoder, registered);
            if (bitstream) nv.nvEncDestroyBitstreamBuffer(encoder, bitstream);
            nv.nvEncDestroyEncoder(encoder);
        }
        encoder = nullptr;
        registered = nullptr;
        bitstream = nullptr;
        vpIn.Reset();
        vpOut.Reset();
        vp.Reset();
        vpEnum.Reset();
        nv12.Reset();
        bgra.Reset();
        dup.Reset();
        output.Reset();
        videoContext.Reset();
        videoDevice.Reset();
        context.Reset();
        device.Reset();
        width = height = encW = encH = 0;
    }

    bool Open(const ScreenConfig& config, std::wstring& error) {
        Close();
        if (!Nvenc().ok) {
            error = Nvenc().error;
            return false;
        }
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            error = L"DXGI 팩토리 생성 실패";
            return false;
        }
        // 이 모니터를 그리는 GPU와 출력 찾기
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 adapterDesc{};
        for (UINT a = 0; !output; ++a) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(a, &candidate) == DXGI_ERROR_NOT_FOUND) break;
            for (UINT o = 0;; ++o) {
                ComPtr<IDXGIOutput> out0;
                if (candidate->EnumOutputs(o, &out0) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC od{};
                out0->GetDesc(&od);
                if (config.device == od.DeviceName && SUCCEEDED(out0.As(&output))) {
                    adapter = candidate;
                    adapter->GetDesc1(&adapterDesc);
                    break;
                }
            }
        }
        if (!output) {
            error = L"모니터를 찾을 수 없음: " + config.device;
            return false;
        }
        if (adapterDesc.VendorId != kNvidiaVendor) {
            error = std::wstring(L"이 모니터는 '") + adapterDesc.Description +
                    L"'에 연결돼 있어 NVENC를 쓸 수 없음 — 가상 모니터(VDD) 설정에서 GPU를 RTX로 고르세요";
            return false;
        }

        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                       _countof(levels), D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(hr) || FAILED(device.As(&videoDevice)) || FAILED(context.As(&videoContext))) {
            error = Format(L"캡처용 D3D11 장치 생성 실패 0x%08X", (unsigned)hr);
            return false;
        }
        if (!OpenDuplication(error)) return false;
        return OpenEncoder(config, error);
    }

    bool OpenDuplication(std::wstring& error) {
        dup.Reset();
        HRESULT hr = output->DuplicateOutput(device.Get(), &dup);
        if (FAILED(hr)) {
            error = hr == E_ACCESSDENIED ? L"화면 캡처가 막힘 (잠금 화면·UAC 창 등) — 잠시 뒤 다시 시도"
                                         : Format(L"화면 캡처 시작 실패 0x%08X", (unsigned)hr);
            return false;
        }
        DXGI_OUTDUPL_DESC dd{};
        dup->GetDesc(&dd);
        if (width && (dd.ModeDesc.Width != width || dd.ModeDesc.Height != height)) {
            error = L"모니터 해상도가 바뀜 — 다시 시작";
            return false;
        }
        width = dd.ModeDesc.Width;
        height = dd.ModeDesc.Height;
        return true;
    }

    bool OpenEncoder(const ScreenConfig& config, std::wstring& error) {
        encW = width & ~1u;
        encH = height & ~1u;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = encW;
        td.Height = encH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, &nv12);
        if (FAILED(hr)) {
            error = Format(L"NV12 텍스처 생성 실패 0x%08X", (unsigned)hr);
            return false;
        }

        auto& nv = Nvenc().api;
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
        open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        open.device = device.Get();
        open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        open.apiVersion = NVENCAPI_VERSION;
        NVENCSTATUS st = nv.nvEncOpenEncodeSessionEx(&open, &encoder);
        if (st != NV_ENC_SUCCESS) {
            encoder = nullptr;
            error = Format(L"NVENC 세션 열기 실패 (%d)", (int)st);
            return false;
        }

        NV_ENC_PRESET_CONFIG preset{};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        st = nv.nvEncGetEncodePresetConfigEx(encoder, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P2_GUID,
                                             NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset);
        if (st != NV_ENC_SUCCESS) {
            error = Format(L"NVENC 프리셋 읽기 실패 (%d)", (int)st);
            return false;
        }
        NV_ENC_CONFIG cfg = preset.presetCfg;
        cfg.version = NV_ENC_CONFIG_VER;
        cfg.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
        cfg.gopLength = NVENC_INFINITE_GOPLENGTH;
        cfg.frameIntervalP = 1;  // B프레임 없음
        uint32_t bitrate = uint32_t(std::max(5, config.bitrateMbps)) * 1000000u;
        int fps = std::max(10, config.fps);
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        cfg.rcParams.averageBitRate = bitrate;
        cfg.rcParams.maxBitRate = bitrate;
        cfg.rcParams.vbvBufferSize = bitrate / uint32_t(fps) * 2;
        cfg.rcParams.vbvInitialDelay = cfg.rcParams.vbvBufferSize;
        auto& h264 = cfg.encodeCodecConfig.h264Config;
        h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        h264.repeatSPSPPS = 1;
        // 색 정보: BT.709, 16-235 (아래 비디오 프로세서 변환과 같게)
        auto& vui = h264.h264VUIParameters;
        vui.videoSignalTypePresentFlag = 1;
        vui.videoFormat = decltype(vui.videoFormat)(5);
        vui.videoFullRangeFlag = 0;
        vui.colourDescriptionPresentFlag = 1;
        vui.colourPrimaries = decltype(vui.colourPrimaries)(1);
        vui.transferCharacteristics = decltype(vui.transferCharacteristics)(1);
        vui.colourMatrix = decltype(vui.colourMatrix)(1);

        NV_ENC_INITIALIZE_PARAMS init{};
        init.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init.encodeGUID = NV_ENC_CODEC_H264_GUID;
        init.presetGUID = NV_ENC_PRESET_P2_GUID;
        init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        init.encodeWidth = encW;
        init.encodeHeight = encH;
        init.darWidth = encW;
        init.darHeight = encH;
        init.maxEncodeWidth = encW;
        init.maxEncodeHeight = encH;
        init.frameRateNum = uint32_t(fps);
        init.frameRateDen = 1;
        init.enablePTD = 1;
        init.encodeConfig = &cfg;
        st = nv.nvEncInitializeEncoder(encoder, &init);
        if (st != NV_ENC_SUCCESS) {
            error = Format(L"NVENC 인코더 설정 실패 (%d)", (int)st);
            return false;
        }

        NV_ENC_CREATE_BITSTREAM_BUFFER bb{};
        bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        st = nv.nvEncCreateBitstreamBuffer(encoder, &bb);
        if (st != NV_ENC_SUCCESS) {
            error = Format(L"NVENC 출력 버퍼 생성 실패 (%d)", (int)st);
            return false;
        }
        bitstream = bb.bitstreamBuffer;

        NV_ENC_REGISTER_RESOURCE reg{};
        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.resourceToRegister = nv12.Get();
        reg.width = encW;
        reg.height = encH;
        reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        reg.bufferUsage = NV_ENC_INPUT_IMAGE;
        st = nv.nvEncRegisterResource(encoder, &reg);
        if (st != NV_ENC_SUCCESS) {
            error = Format(L"NVENC 입력 등록 실패 (%d)", (int)st);
            return false;
        }
        registered = reg.registeredResource;
        return true;
    }

    enum class Result { Frame, Idle, Lost, Fail };

    Result Acquire(UINT timeoutMs, std::wstring& error) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = dup->AcquireNextFrame(timeoutMs, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::Idle;
        if (hr == DXGI_ERROR_ACCESS_LOST) return Result::Lost;
        if (FAILED(hr)) {
            error = Format(L"화면 프레임 받기 실패 0x%08X", (unsigned)hr);
            return Result::Fail;
        }
        Result result = Result::Idle;
        // LastPresentTime이 0이면 마우스만 움직인 것
        if (info.LastPresentTime.QuadPart != 0) {
            ComPtr<ID3D11Texture2D> frame;
            if (SUCCEEDED(resource.As(&frame)) && EnsureConverter(frame.Get(), error)) {
                context->CopyResource(bgra.Get(), frame.Get());
                result = Result::Frame;
            } else if (!error.empty()) {
                result = Result::Fail;
            }
        }
        dup->ReleaseFrame();
        return result;
    }

    // 첫 프레임의 형식에 맞춰 복사본과 BGRA → NV12 변환기를 만든다
    bool EnsureConverter(ID3D11Texture2D* frame, std::wstring& error) {
        if (bgra) return true;
        D3D11_TEXTURE2D_DESC fd{};
        frame->GetDesc(&fd);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = fd.Width;
        td.Height = fd.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fd.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, &bgra);
        if (FAILED(hr)) {
            error = Format(L"캡처 복사본 생성 실패 0x%08X (형식 %d)", (unsigned)hr, (int)fd.Format);
            return false;
        }
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = fd.Width;
        cd.InputHeight = fd.Height;
        cd.InputFrameRate = {60, 1};
        cd.OutputWidth = encW;
        cd.OutputHeight = encH;
        cd.OutputFrameRate = {60, 1};
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        hr = videoDevice->CreateVideoProcessorEnumerator(&cd, &vpEnum);
        if (SUCCEEDED(hr)) hr = videoDevice->CreateVideoProcessor(vpEnum.Get(), 0, &vp);
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (SUCCEEDED(hr)) hr = videoDevice->CreateVideoProcessorInputView(bgra.Get(), vpEnum.Get(), &iv, &vpIn);
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
        ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (SUCCEEDED(hr)) hr = videoDevice->CreateVideoProcessorOutputView(nv12.Get(), vpEnum.Get(), &ov, &vpOut);
        if (FAILED(hr)) {
            bgra.Reset();
            error = Format(L"색 변환기 생성 실패 0x%08X", (unsigned)hr);
            return false;
        }
        RECT src{0, 0, (LONG)fd.Width, (LONG)fd.Height};
        RECT dst{0, 0, (LONG)encW, (LONG)encH};
        videoContext->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
        videoContext->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        videoContext->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &src);
        videoContext->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst);
        videoContext->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dst);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inSpace{};
        inSpace.RGB_Range = 0;  // 데스크톱은 0-255
        videoContext->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &inSpace);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outSpace{};
        outSpace.YCbCr_Matrix = 1;  // BT.709
        outSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        videoContext->VideoProcessorSetOutputColorSpace(vp.Get(), &outSpace);
        return true;
    }

    bool Convert(std::wstring& error) {
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = vpIn.Get();
        HRESULT hr = videoContext->VideoProcessorBlt(vp.Get(), vpOut.Get(), 0, 1, &stream);
        if (FAILED(hr)) {
            error = Format(L"색 변환 실패 0x%08X", (unsigned)hr);
            return false;
        }
        return true;
    }

    bool Encode(bool idr, uint64_t index, bool& key, std::wstring& error) {
        auto& nv = Nvenc().api;
        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = registered;
        NVENCSTATUS st = nv.nvEncMapInputResource(encoder, &map);
        if (st != NV_ENC_SUCCESS) {
            error = Format(L"NVENC 입력 연결 실패 (%d)", (int)st);
            return false;
        }
        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputWidth = encW;
        pic.inputHeight = encH;
        pic.inputBuffer = map.mappedResource;
        pic.bufferFmt = map.mappedBufferFmt;
        pic.outputBitstream = bitstream;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputTimeStamp = index;
        if (idr) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        st = nv.nvEncEncodePicture(encoder, &pic);
        bool ok = st == NV_ENC_SUCCESS;
        if (ok) {
            NV_ENC_LOCK_BITSTREAM lock{};
            lock.version = NV_ENC_LOCK_BITSTREAM_VER;
            lock.outputBitstream = bitstream;
            st = nv.nvEncLockBitstream(encoder, &lock);
            ok = st == NV_ENC_SUCCESS;
            if (ok) {
                const uint8_t* p = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
                out.assign(p, p + lock.bitstreamSizeInBytes);
                key = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
                nv.nvEncUnlockBitstream(encoder, bitstream);
            }
        }
        nv.nvEncUnmapInputResource(encoder, map.mappedResource);
        if (!ok) error = Format(L"NVENC 인코딩 실패 (%d)", (int)st);
        return ok;
    }
};
}  // namespace

void ScreenSender::Start(const ScreenConfig& config, FrameCallback onFrame) {
    Stop();
    config_ = config;
    onFrame_ = std::move(onFrame);
    stop_ = false;
    keyRequested_ = true;
    SetError(L"");
    thread_ = std::thread([this] { Thread(); });
}

void ScreenSender::Stop() {
    stop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    width = 0;
    height = 0;
}

std::wstring ScreenSender::Error() {
    std::lock_guard lock(mutex_);
    return error_;
}

void ScreenSender::SetError(const std::wstring& e) {
    std::lock_guard lock(mutex_);
    error_ = e;
}

bool ScreenSender::Wait(int ms) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return stop_.load(); });
}

void ScreenSender::Thread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Capture cap;
        bool open = false;
        bool pending = false;     // 받았지만 아직 인코딩 안 한 프레임
        bool haveEncoded = false;
        uint64_t index = 0;
        auto lastEncode = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        const auto interval = std::chrono::microseconds(1000000 / std::max(10, config_.fps));
        std::wstring lastError;

        while (!stop_) {
            std::wstring error;
            if (!open) {
                if (!cap.Open(config_, error)) {
                    if (error != lastError) Log(L"화면 보내기: " + error);
                    lastError = error;
                    SetError(error);
                    cap.Close();
                    if (Wait(2000)) break;
                    continue;
                }
                open = true;
                pending = false;
                haveEncoded = false;
                keyRequested_ = true;
                lastError.clear();
                SetError(L"");
                width = int(cap.encW);
                height = int(cap.encH);
                Log(Format(L"화면 보내기 시작: %ls %ux%u, %dMbps", config_.device.c_str(), cap.encW, cap.encH,
                           config_.bitrateMbps));
            }

            auto result = cap.Acquire(16, error);
            if (result == Capture::Result::Lost) {
                // 해상도 변경, 전체 화면 전환, UAC 창 등
                if (!cap.OpenDuplication(error)) {
                    Log(L"화면 보내기: " + error + L" — 다시 연결");
                    open = false;
                    cap.Close();
                    if (Wait(500)) break;
                }
                keyRequested_ = true;
                continue;
            }
            if (result == Capture::Result::Fail) {
                Log(L"화면 보내기: " + error + L" — 다시 시작");
                SetError(error);
                open = false;
                cap.Close();
                if (Wait(500)) break;
                continue;
            }
            if (result == Capture::Result::Frame) pending = true;

            auto now = std::chrono::steady_clock::now();
            bool encode = false;
            if (pending && now - lastEncode >= interval) encode = true;
            // 화면이 멈춰 있어도 새로 붙은 iPad가 키프레임을 받게 마지막 화면을 다시 인코딩
            if (!pending && haveEncoded && keyRequested_ && now - lastEncode > std::chrono::milliseconds(100))
                encode = true;
            if (!encode) continue;

            if (pending && !cap.Convert(error)) {
                Log(L"화면 보내기: " + error);
                open = false;
                cap.Close();
                continue;
            }
            bool idr = keyRequested_.exchange(false) || !haveEncoded;
            bool key = false;
            if (!cap.Encode(idr, index++, key, error)) {
                Log(L"화면 보내기: " + error + L" — 다시 시작");
                SetError(error);
                open = false;
                cap.Close();
                if (Wait(500)) break;
                continue;
            }
            pending = false;
            haveEncoded = true;
            lastEncode = now;
            ++frames;
            bytes += cap.out.size();
            if (onFrame_ && !cap.out.empty()) onFrame_(cap.out.data(), cap.out.size(), key);
        }
    }
    CoUninitialize();
}
