#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class LinkMode { Usb = 0, Wifi = 1 };

struct ReceiverCallbacks {
    std::function<void(std::vector<uint8_t>&& annexB, bool key, uint8_t orientation)> onVideo;
    std::function<void(const int16_t* pcm, size_t frames, int sampleRate)> onAudio;
    std::function<void()> onConnected;  // 연결 직후 (Send 가능)
    std::function<void(const std::string& status)> onStatus;
};

// iPad 방송 확장(포트 47800)에 접속해서 프레임을 읽는다. 끊기면 계속 다시 붙는다.
class Receiver {
public:
    ~Receiver() { Stop(); }
    void Start(LinkMode mode, std::string ip, uint16_t port, ReceiverCallbacks callbacks);
    void Stop();
    bool Running() const { return thread_.joinable(); }
    bool Send(uint8_t kind, const void* data, size_t len);

    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> videoFrames{0};
    std::atomic<bool> connected{false};

private:
    void Loop();
    int Open(std::string& why);
    int OpenWifi(std::string& why);
    int OpenUsb(std::string& why);
    void ReadLoop(int fd);

    LinkMode mode_ = LinkMode::Usb;
    std::string ip_;
    uint16_t port_ = 0;
    ReceiverCallbacks cb_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex socketMutex_;
    std::mutex sendMutex_;
    int socket_ = -1;
    std::mutex waitMutex_;
    std::condition_variable waitCv_;
};
