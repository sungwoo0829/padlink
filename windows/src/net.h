#pragma once
#include "common.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

enum class LinkMode { Usb = 0, Wifi = 1 };

struct ReceiverCallbacks {
    std::function<void(std::vector<uint8_t>&& annexB, bool key, uint8_t orientation, uint64_t ptsUs)> onVideo;
    std::function<void(const int16_t* pcm, size_t frames, int sampleRate)> onAudio;
    std::function<void(const uint8_t* data, size_t len)> onPenSamples;
    std::function<void(uint8_t button, uint8_t phase)> onPenButton;
    std::function<void()> onConnected;     // 연결 직후 (Send 가능)
    std::function<void()> onDisconnected;
    std::function<void(const std::wstring& status)> onStatus;
};

// iPad의 한 포트에 접속해서 프레임을 읽는다. 끊기면 계속 다시 붙는다.
class Receiver {
public:
    ~Receiver() { Stop(); }
    // label: 로그 머리말, waitHint: 기기 쪽 포트가 닫혀 있을 때 안내
    void Start(LinkMode mode, std::wstring ip, uint16_t port, std::wstring label, std::wstring waitHint,
               ReceiverCallbacks callbacks);
    void Stop();
    bool Running() const { return thread_.joinable(); }
    bool Send(uint8_t kind, const std::string& payload);

    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> videoFrames{0};
    std::atomic<bool> connected{false};

private:
    void Loop();
    SOCKET Open(std::wstring& why);
    SOCKET OpenWifi(std::wstring& why);
    SOCKET OpenUsb(std::wstring& why);
    void ReadLoop(SOCKET s);

    LinkMode mode_ = LinkMode::Usb;
    std::wstring ip_;
    uint16_t port_ = wire::kPort;
    std::wstring label_;
    std::wstring waitHint_;
    ReceiverCallbacks cb_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex socketMutex_;
    SOCKET socket_ = INVALID_SOCKET;
    std::mutex waitMutex_;
    std::condition_variable waitCv_;
};
