#include "video.h"

#include <codecapi.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "log.h"

namespace {
// CGImagePropertyOrientation(iPad) → 시계 방향 90도 회전 횟수
int OrientationToTurns(uint8_t orientation) {
    switch (orientation) {
    case 3: return 2;  // down
    case 6: return 1;  // right: 시계 방향 90도로 돌려야 똑바로 보임
    case 8: return 3;  // left
    default: return 0;
    }
}
}  // namespace

bool VideoPipeline::Start(HWND hwnd, int userRotation) {
    hwnd_ = hwnd;
    userRotation_ = userRotation & 3;

    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) {
        Log(Format(L"DXGI 팩토리 생성 실패 0x%08X", (unsigned)hr));
        return false;
    }
    // 내장 그래픽이 켜져 있어도 RTX(고성능 GPU)를 쓴다
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory_.As(&factory6)))
        factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    if (!adapter) factory_->EnumAdapters1(0, &adapter);
    if (adapter) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        adapterName = desc.Description;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           flags, levels, _countof(levels), D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) {
        Log(Format(L"D3D11 장치 생성 실패 0x%08X", (unsigned)hr));
        return false;
    }
    // 디코더(MF)와 렌더링이 같은 장치를 쓰므로 보호를 켠다
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    if (FAILED(device_.As(&videoDevice_)) || FAILED(context_.As(&videoContext_))) {
        Log(L"이 GPU는 D3D11 비디오 기능을 지원하지 않음");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = factory_->CreateSwapChainForHwnd(device_.Get(), hwnd_, &sd, nullptr, nullptr, &swap_);
    if (FAILED(hr)) {
        Log(Format(L"스왑체인 생성 실패 0x%08X", (unsigned)hr));
        return false;
    }
    factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    DXGI_SWAP_CHAIN_DESC1 actual{};
    swap_->GetDesc1(&actual);
    swapW_ = actual.Width;
    swapH_ = actual.Height;

    Log(L"그래픽: " + adapterName);
    thread_ = std::thread([this] { Thread(); });
    return true;
}

void VideoPipeline::Stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void VideoPipeline::Push(std::vector<uint8_t>&& annexB, bool key, uint8_t orientation, uint64_t ptsUs) {
    {
        std::lock_guard lock(mutex_);
        if (queue_.size() > 20) {
            // 디코딩이 밀리면 쌓인 걸 버리고 다음 키프레임부터
            queue_.clear();
            waitKey_ = true;
        }
        queue_.push_back(Packet{std::move(annexB), key, orientation, ptsUs});
    }
    cv_.notify_one();
}

void VideoPipeline::RequestRedraw() {
    redraw_ = true;
    cv_.notify_one();
}

int VideoPipeline::RotateUser() {
    int r = (userRotation_ + 1) & 3;
    userRotation_ = r;
    RequestRedraw();
    return r;
}

void VideoPipeline::RequestSnapshot(SnapshotCallback callback) {
    {
        std::lock_guard lock(snapshotMutex_);
        snapshotCallback_ = std::move(callback);
    }
    snapshotRequested_ = true;
    cv_.notify_one();
}

void VideoPipeline::ToggleRange() {
    rangeFlip_ = !rangeFlip_;
    RequestRedraw();
}

void VideoPipeline::Thread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    bool decoderOk = InitDecoder();

    for (;;) {
        Packet packet;
        bool havePacket = false;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
                return stop_ || !queue_.empty() || redraw_.load() || snapshotRequested_.load();
            });
            if (stop_) break;
            if (!queue_.empty()) {
                packet = std::move(queue_.front());
                queue_.pop_front();
                havePacket = true;
                if (waitKey_) {
                    if (!packet.key) havePacket = false;
                    else waitKey_ = false;
                }
            }
        }
        if (havePacket && decoderOk) {
            Decode(packet);
        } else if (redraw_.exchange(false)) {
            ApplyResize();
            if (lastSample_) Render();
            else ClearBlack();
        }
        if (snapshotRequested_.exchange(false)) DoSnapshot();
    }

    lastSample_.Reset();
    inputViews_.clear();
    if (decoder_) {
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        decoder_.Reset();
    }
    deviceManager_.Reset();
    MFShutdown();
    CoUninitialize();
}

bool VideoPipeline::InitDecoder() {
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder_));
    if (FAILED(hr)) {
        Log(Format(L"H.264 디코더 생성 실패 0x%08X", (unsigned)hr));
        return false;
    }
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(decoder_->GetAttributes(&attrs))) {
        attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        UINT32 d3dAware = 0;
        attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
        if (d3dAware) {
            UINT token = 0;
            hr = MFCreateDXGIDeviceManager(&token, &deviceManager_);
            if (SUCCEEDED(hr)) hr = deviceManager_->ResetDevice(device_.Get(), token);
            if (SUCCEEDED(hr))
                hr = decoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                              reinterpret_cast<ULONG_PTR>(deviceManager_.Get()));
            if (FAILED(hr)) Log(Format(L"하드웨어 디코딩 설정 실패 0x%08X", (unsigned)hr));
        } else {
            Log(L"디코더가 D3D11을 지원하지 않음");
        }
    }

    ComPtr<IMFMediaType> input;
    MFCreateMediaType(&input);
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = decoder_->SetInputType(0, input.Get(), 0);
    if (FAILED(hr)) {
        Log(Format(L"디코더 입력 형식 설정 실패 0x%08X", (unsigned)hr));
        return false;
    }
    // 첫 SPS가 오기 전 기본 형식(1920x1080)은 실제 영상이 아니므로 로그에 남기지 않는다
    if (!NegotiateOutput(false)) return false;
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool VideoPipeline::NegotiateOutput(bool announce) {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(decoder_->GetOutputAvailableType(0, i, &type))) break;
        GUID subtype{};
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype != MFVideoFormat_NV12) continue;
        HRESULT hr = decoder_->SetOutputType(0, type.Get(), 0);
        if (FAILED(hr)) {
            Log(Format(L"디코더 출력 형식 설정 실패 0x%08X", (unsigned)hr));
            return false;
        }
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        frameW_ = w;
        frameH_ = h;
        crop_ = RECT{0, 0, (LONG)w, (LONG)h};
        MFVideoArea area{};
        UINT32 blobSize = 0;
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area),
                                    &blobSize)) &&
            area.Area.cx > 0 && area.Area.cy > 0) {
            crop_ = RECT{area.OffsetX.value, area.OffsetY.value, area.OffsetX.value + area.Area.cx,
                         area.OffsetY.value + area.Area.cy};
        }
        UINT32 range = 0;
        // 정보가 없으면 전체 범위로 본다 (iPad 화면 버퍼가 full range)
        fullRange_ = FAILED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range)) || range != MFNominalRange_16_235;
        width = crop_.right - crop_.left;
        height = crop_.bottom - crop_.top;
        inputViews_.clear();
        if (announce && w > 0) Log(Format(L"영상 %ux%u (표시 %dx%d, %ls)", w, h, width.load(), height.load(),
                              fullRange_ ? L"0-255" : L"16-235"));
        return true;
    }
    Log(L"디코더가 NV12 출력을 지원하지 않음");
    return false;
}

void VideoPipeline::Decode(Packet& p) {
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer((DWORD)p.data.size(), &buffer))) return;
    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) return;
    std::memcpy(dst, p.data.data(), p.data.size());
    buffer->Unlock();
    buffer->SetCurrentLength((DWORD)p.data.size());
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime((LONGLONG)p.pts * 10);
    if (p.key) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    orientation_ = p.orientation;

    HRESULT hr = decoder_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        Drain();
        hr = decoder_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        Log(Format(L"디코더 입력 실패 0x%08X", (unsigned)hr));
        return;
    }
    Drain();
}

void VideoPipeline::Drain() {
    for (;;) {
        MFT_OUTPUT_STREAM_INFO info{};
        decoder_->GetOutputStreamInfo(0, &info);
        bool decoderAllocates =
            (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        MFT_OUTPUT_DATA_BUFFER out{};
        out.dwStreamID = 0;
        ComPtr<IMFSample> own;
        if (!decoderAllocates) {
            ComPtr<IMFMediaBuffer> buffer;
            MFCreateSample(&own);
            MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 1), &buffer);
            own->AddBuffer(buffer.Get());
            out.pSample = own.Get();
        }
        DWORD status = 0;
        HRESULT hr = decoder_->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (decoderAllocates && out.pSample) out.pSample->Release();
            if (!NegotiateOutput()) return;
            continue;
        }
        if (FAILED(hr)) {
            Log(Format(L"디코더 출력 실패 0x%08X", (unsigned)hr));
            return;
        }
        ComPtr<IMFSample> frame;
        if (decoderAllocates) frame.Attach(out.pSample);
        else frame = own;
        if (!frame) continue;
        lastSample_ = frame;
        ++decodedFrames;
        ApplyResize();
        Render();
    }
}

void VideoPipeline::ApplyResize() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT w = (UINT)std::max<LONG>(rc.right - rc.left, 0);
    UINT h = (UINT)std::max<LONG>(rc.bottom - rc.top, 0);
    if (w == 0 || h == 0 || (w == swapW_ && h == swapH_)) return;
    outputView_.Reset();
    context_->ClearState();
    context_->Flush();
    HRESULT hr = swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        Log(Format(L"화면 크기 변경 실패 0x%08X", (unsigned)hr));
        return;
    }
    swapW_ = w;
    swapH_ = h;
}

bool VideoPipeline::EnsureProcessor(UINT inW, UINT inH) {
    bool same = processor_ && inW == vpInW_ && inH == vpInH_ && swapW_ == vpOutW_ && swapH_ == vpOutH_;
    if (!same) {
        inputViews_.clear();
        outputView_.Reset();
        processor_.Reset();
        vpEnum_.Reset();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate = {60, 1};
        cd.InputWidth = inW;
        cd.InputHeight = inH;
        cd.OutputFrameRate = {60, 1};
        cd.OutputWidth = swapW_;
        cd.OutputHeight = swapH_;
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT hr = videoDevice_->CreateVideoProcessorEnumerator(&cd, &vpEnum_);
        if (SUCCEEDED(hr)) hr = videoDevice_->CreateVideoProcessor(vpEnum_.Get(), 0, &processor_);
        if (FAILED(hr)) {
            Log(Format(L"비디오 프로세서 생성 실패 0x%08X", (unsigned)hr));
            processor_.Reset();
            return false;
        }
        D3D11_VIDEO_PROCESSOR_CAPS caps{};
        vpEnum_->GetVideoProcessorCaps(&caps);
        canRotate_ = (caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION) != 0;
        videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
        videoContext_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        vpInW_ = inW;
        vpInH_ = inH;
        vpOutW_ = swapW_;
        vpOutH_ = swapH_;
    }
    if (!outputView_) {
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
        od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        HRESULT hr = videoDevice_->CreateVideoProcessorOutputView(back.Get(), vpEnum_.Get(), &od, &outputView_);
        if (FAILED(hr)) {
            Log(Format(L"출력 뷰 생성 실패 0x%08X", (unsigned)hr));
            return false;
        }
    }
    return true;
}

ID3D11VideoProcessorInputView* VideoPipeline::GetInputView(ID3D11Texture2D* texture, UINT slice) {
    for (const InputView& v : inputViews_)
        if (v.texture.Get() == texture && v.slice == slice) return v.view.Get();
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
    iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    iv.Texture2D.ArraySlice = slice;
    InputView entry;
    entry.texture = texture;
    entry.slice = slice;
    HRESULT hr = videoDevice_->CreateVideoProcessorInputView(texture, vpEnum_.Get(), &iv, &entry.view);
    if (FAILED(hr)) {
        Log(Format(L"입력 뷰 생성 실패 0x%08X", (unsigned)hr));
        return nullptr;
    }
    inputViews_.push_back(entry);
    return entry.view.Get();
}

void VideoPipeline::Render() {
    if (!lastSample_ || swapW_ == 0 || swapH_ == 0) return;
    ComPtr<ID3D11Texture2D> texture;
    UINT slice = 0;
    if (!CurrentFrame(texture, slice)) return;
    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);
    if (!EnsureProcessor(td.Width, td.Height)) return;
    ID3D11VideoProcessorInputView* input = GetInputView(texture.Get(), slice);
    if (!input) return;

    RECT src = crop_;
    src.right = std::min<LONG>(src.right, (LONG)td.Width);
    src.bottom = std::min<LONG>(src.bottom, (LONG)td.Height);
    int turns = (OrientationToTurns(orientation_) + userRotation_) & 3;
    if (!canRotate_) turns = 0;
    float cw = float(src.right - src.left), ch = float(src.bottom - src.top);
    if (turns & 1) std::swap(cw, ch);
    if (cw <= 0 || ch <= 0) return;
    shownWidth = int(cw);
    shownHeight = int(ch);
    float scale = std::min(swapW_ / cw, swapH_ / ch);
    LONG dw = LONG(cw * scale), dh = LONG(ch * scale);
    RECT dst{LONG(swapW_ - dw) / 2, LONG(swapH_ - dh) / 2, 0, 0};
    dst.right = dst.left + dw;
    dst.bottom = dst.top + dh;
    RECT full{0, 0, (LONG)swapW_, (LONG)swapH_};

    ID3D11VideoProcessor* vp = processor_.Get();
    videoContext_->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &src);
    videoContext_->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &dst);
    videoContext_->VideoProcessorSetOutputTargetRect(vp, TRUE, &full);
    if (canRotate_)
        videoContext_->VideoProcessorSetStreamRotation(vp, 0, turns != 0, (D3D11_VIDEO_PROCESSOR_ROTATION)turns);

    bool full255 = fullRange_ != rangeFlip_.load();
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inSpace{};
    inSpace.YCbCr_Matrix = 1;  // BT.709
    inSpace.Nominal_Range = full255 ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetStreamColorSpace(vp, 0, &inSpace);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outSpace{};
    outSpace.RGB_Range = 0;  // 0-255
    videoContext_->VideoProcessorSetOutputColorSpace(vp, &outSpace);
    D3D11_VIDEO_COLOR black{};
    black.RGBA.A = 1.0f;
    videoContext_->VideoProcessorSetOutputBackgroundColor(vp, FALSE, &black);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input;
    HRESULT hr = videoContext_->VideoProcessorBlt(vp, outputView_.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        Log(Format(L"화면 변환 실패 0x%08X", (unsigned)hr));
        return;
    }
    swap_->Present(1, 0);
}

bool VideoPipeline::CurrentFrame(ComPtr<ID3D11Texture2D>& texture, UINT& slice) {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    if (!lastSample_ || FAILED(lastSample_->GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&dxgiBuffer)) ||
        FAILED(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture))) || FAILED(dxgiBuffer->GetSubresourceIndex(&slice))) {
        if (lastSample_ && !warnedSoftware_) {
            warnedSoftware_ = true;
            Log(L"디코더가 GPU 텍스처를 주지 않음 (하드웨어 디코딩 아님) — 화면을 그릴 수 없음");
        }
        return false;
    }
    return true;
}

// 화면에 보이는 것과 같은 방향·색으로, 잘라낸 원본 크기 그대로 BGRA 텍스처에 그려서 읽어 온다
void VideoPipeline::DoSnapshot() {
    SnapshotCallback callback;
    {
        std::lock_guard lock(snapshotMutex_);
        callback = std::move(snapshotCallback_);
        snapshotCallback_ = nullptr;
    }
    if (!callback) return;
    std::vector<uint8_t> pixels;
    int outW = 0, outH = 0;
    ComPtr<ID3D11Texture2D> texture;
    UINT slice = 0;
    if (!CurrentFrame(texture, slice)) {
        callback(std::move(pixels), 0, 0);
        return;
    }
    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);
    RECT src = crop_;
    src.right = std::min<LONG>(src.right, (LONG)td.Width);
    src.bottom = std::min<LONG>(src.bottom, (LONG)td.Height);
    UINT cw = UINT(src.right - src.left), ch = UINT(src.bottom - src.top);

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {60, 1};
    cd.InputWidth = td.Width;
    cd.InputHeight = td.Height;
    cd.OutputFrameRate = {60, 1};
    cd.OutputWidth = std::max(cw, ch);
    cd.OutputHeight = std::max(cw, ch);
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
    ComPtr<ID3D11VideoProcessorEnumerator> en;
    ComPtr<ID3D11VideoProcessor> vp;
    HRESULT hr = videoDevice_->CreateVideoProcessorEnumerator(&cd, &en);
    if (SUCCEEDED(hr)) hr = videoDevice_->CreateVideoProcessor(en.Get(), 0, &vp);
    if (FAILED(hr)) {
        Log(Format(L"복사용 변환기 생성 실패 0x%08X", (unsigned)hr));
        callback(std::move(pixels), 0, 0);
        return;
    }
    D3D11_VIDEO_PROCESSOR_CAPS caps{};
    en->GetVideoProcessorCaps(&caps);
    bool rotate = (caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION) != 0;
    int turns = rotate ? (OrientationToTurns(orientation_) + userRotation_) & 3 : 0;
    outW = int(turns & 1 ? ch : cw);
    outH = int(turns & 1 ? cw : ch);

    D3D11_TEXTURE2D_DESC od{};
    od.Width = UINT(outW);
    od.Height = UINT(outH);
    od.MipLevels = 1;
    od.ArraySize = 1;
    od.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    od.SampleDesc.Count = 1;
    od.Usage = D3D11_USAGE_DEFAULT;
    od.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, staging;
    hr = device_->CreateTexture2D(&od, nullptr, &target);
    od.Usage = D3D11_USAGE_STAGING;
    od.BindFlags = 0;
    od.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = device_->CreateTexture2D(&od, nullptr, &staging);

    ComPtr<ID3D11VideoProcessorInputView> inView;
    ComPtr<ID3D11VideoProcessorOutputView> outView;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
    iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    iv.Texture2D.ArraySlice = slice;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
    ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (SUCCEEDED(hr)) hr = videoDevice_->CreateVideoProcessorInputView(texture.Get(), en.Get(), &iv, &inView);
    if (SUCCEEDED(hr)) hr = videoDevice_->CreateVideoProcessorOutputView(target.Get(), en.Get(), &ov, &outView);
    if (FAILED(hr)) {
        Log(Format(L"복사용 화면 준비 실패 0x%08X", (unsigned)hr));
        callback(std::move(pixels), 0, 0);
        return;
    }

    RECT dst{0, 0, outW, outH};
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
    videoContext_->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &src);
    videoContext_->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst);
    videoContext_->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dst);
    if (rotate) videoContext_->VideoProcessorSetStreamRotation(vp.Get(), 0, turns != 0, (D3D11_VIDEO_PROCESSOR_ROTATION)turns);
    bool full255 = fullRange_ != rangeFlip_.load();
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inSpace{};
    inSpace.YCbCr_Matrix = 1;
    inSpace.Nominal_Range = full255 ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &inSpace);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outSpace{};
    videoContext_->VideoProcessorSetOutputColorSpace(vp.Get(), &outSpace);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inView.Get();
    hr = videoContext_->VideoProcessorBlt(vp.Get(), outView.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        Log(Format(L"복사용 화면 변환 실패 0x%08X", (unsigned)hr));
        callback(std::move(pixels), 0, 0);
        return;
    }
    context_->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        callback(std::move(pixels), 0, 0);
        return;
    }
    const size_t rowBytes = size_t(outW) * 4;
    pixels.resize(rowBytes * size_t(outH));
    for (int y = 0; y < outH; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
        std::memcpy(pixels.data() + size_t(y) * rowBytes, row, rowBytes);
    }
    context_->Unmap(staging.Get(), 0);
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;  // 알파는 불투명으로
    callback(std::move(pixels), outW, outH);
}

void VideoPipeline::ClearBlack() {
    if (swapW_ == 0 || swapH_ == 0) return;
    ComPtr<ID3D11Texture2D> back;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back))) ||
        FAILED(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv)))
        return;
    const float color[4] = {0, 0, 0, 1};
    context_->ClearRenderTargetView(rtv.Get(), color);
    swap_->Present(1, 0);
}
