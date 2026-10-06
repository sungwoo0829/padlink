#pragma once
#include "common.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// H.264(Annex-B) → Media Foundation 하드웨어 디코더 → D3D11 비디오 프로세서로 회전·맞춤 → 창
class VideoPipeline {
public:
    bool Start(HWND hwnd, int userRotation);
    void Stop();
    void Push(std::vector<uint8_t>&& annexB, bool key, uint8_t orientation, uint64_t ptsUs);
    void RequestRedraw();
    int RotateUser();     // 90도 더 돌리고 새 값(0~3)을 돌려준다
    void ToggleRange();   // 색 범위(16-235 / 0-255) 수동 전환

    // 지금 화면을 원본 해상도로(창 크기·검은 여백과 상관없이) BGRA로 뽑는다. 영상 스레드에서 콜백.
    using SnapshotCallback = std::function<void(std::vector<uint8_t>&& bgra, int width, int height)>;
    void RequestSnapshot(SnapshotCallback callback);

    std::wstring adapterName;
    std::atomic<int> width{0};
    std::atomic<int> height{0};
    // 회전까지 반영해 화면에 보이는 영상 크기 (창을 영상 비율에 맞출 때 씀)
    std::atomic<int> shownWidth{0};
    std::atomic<int> shownHeight{0};
    std::atomic<uint64_t> decodedFrames{0};

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    struct Packet {
        std::vector<uint8_t> data;
        bool key = false;
        uint8_t orientation = 1;
        uint64_t pts = 0;
    };
    struct InputView {
        ComPtr<ID3D11Texture2D> texture;
        UINT slice = 0;
        ComPtr<ID3D11VideoProcessorInputView> view;
    };

    void Thread();
    bool InitDecoder();
    bool NegotiateOutput(bool announce = true);
    void Decode(Packet& p);
    void Drain();
    void Render();
    void ClearBlack();
    void DoSnapshot();
    bool CurrentFrame(ComPtr<ID3D11Texture2D>& texture, UINT& slice);
    void ApplyResize();
    bool EnsureProcessor(UINT inW, UINT inH);
    ID3D11VideoProcessorInputView* GetInputView(ID3D11Texture2D* texture, UINT slice);

    HWND hwnd_ = nullptr;
    ComPtr<IDXGIFactory2> factory_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<IDXGISwapChain1> swap_;
    UINT swapW_ = 0, swapH_ = 0;

    ComPtr<IMFTransform> decoder_;
    ComPtr<IMFDXGIDeviceManager> deviceManager_;
    UINT frameW_ = 0, frameH_ = 0;
    RECT crop_{};
    bool fullRange_ = true;
    bool warnedSoftware_ = false;

    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11VideoProcessorOutputView> outputView_;
    UINT vpInW_ = 0, vpInH_ = 0, vpOutW_ = 0, vpOutH_ = 0;
    bool canRotate_ = false;
    std::vector<InputView> inputViews_;

    ComPtr<IMFSample> lastSample_;
    uint8_t orientation_ = 1;
    std::atomic<int> userRotation_{0};
    std::atomic<bool> rangeFlip_{false};

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Packet> queue_;
    bool waitKey_ = true;
    bool stop_ = false;
    std::atomic<bool> redraw_{true};
    std::atomic<bool> snapshotRequested_{false};
    std::mutex snapshotMutex_;
    SnapshotCallback snapshotCallback_;
};
