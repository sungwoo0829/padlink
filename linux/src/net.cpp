#include "net.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "log.h"
#include "usbmux.h"
#include "wire.h"

void Receiver::Start(LinkMode mode, std::string ip, uint16_t port, ReceiverCallbacks callbacks) {
    Stop();
    mode_ = mode;
    ip_ = std::move(ip);
    port_ = port;
    cb_ = std::move(callbacks);
    stop_ = false;
    thread_ = std::thread([this] { Loop(); });
}

void Receiver::Stop() {
    stop_ = true;
    {
        std::lock_guard lock(socketMutex_);
        if (socket_ >= 0) shutdown(socket_, SHUT_RDWR);
    }
    waitCv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool Receiver::Send(uint8_t kind, const void* data, size_t len) {
    std::lock_guard sendLock(sendMutex_);
    int fd = -1;
    {
        std::lock_guard lock(socketMutex_);
        fd = socket_;
    }
    if (fd < 0) return false;
    wire::Header h{kind, 0, 0, uint32_t(len), 0};
    return SendAll(fd, &h, sizeof(h)) && (len == 0 || SendAll(fd, data, len));
}

void Receiver::Loop() {
    std::string lastWhy;
    while (!stop_) {
        std::string why;
        int fd = Open(why);
        if (fd < 0) {
            if (why != lastWhy) {
                Log("방송: " + why);
                if (cb_.onStatus) cb_.onStatus(why);
                lastWhy = why;
            }
            std::unique_lock lock(waitMutex_);
            waitCv_.wait_for(lock, std::chrono::milliseconds(1500), [this] { return stop_.load(); });
            continue;
        }
        lastWhy.clear();
        {
            std::lock_guard lock(socketMutex_);
            if (stop_) {
                close(fd);
                break;
            }
            socket_ = fd;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        int rcvbuf = 4 << 20;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        std::string how = mode_ == LinkMode::Usb ? "USB" : "Wi-Fi";
        connected = true;
        Log("방송: " + how + "로 iPad에 연결됨");
        if (cb_.onStatus) cb_.onStatus(how + " 연결됨");
        if (cb_.onConnected) cb_.onConnected();
        ReadLoop(fd);
        connected = false;
        {
            std::lock_guard lock(socketMutex_);
            socket_ = -1;
        }
        // 다른 스레드가 보내는 중이면 끝날 때까지 기다린 뒤 닫는다
        shutdown(fd, SHUT_RDWR);
        { std::lock_guard sendLock(sendMutex_); }
        close(fd);
        if (!stop_) {
            Log("방송: 연결 끊김, 다시 연결 중");
            if (cb_.onStatus) cb_.onStatus("연결 끊김, 다시 연결 중…");
        }
    }
}

int Receiver::Open(std::string& why) { return mode_ == LinkMode::Usb ? OpenUsb(why) : OpenWifi(why); }

int Receiver::OpenWifi(std::string& why) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    std::string port = std::to_string(port_);
    if (getaddrinfo(ip_.c_str(), port.c_str(), &hints, &result) != 0 || !result) {
        why = "IP 주소를 해석할 수 없음: " + ip_;
        return -1;
    }
    int fd = socket(result->ai_family, result->ai_socktype | SOCK_NONBLOCK, result->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(result);
        why = "소켓 생성 실패";
        return -1;
    }
    // 2초 안에 안 붙으면 포기 (방송이 꺼져 있으면 바로 거부됨)
    int r = connect(fd, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    if (r != 0 && errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        r = poll(&p, 1, 2000);
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (r == 0) {
            close(fd);
            why = "Wi-Fi: " + ip_ + " 응답 없음 (같은 네트워크인지 확인)";
            return -1;
        }
        r = err == 0 ? 0 : -1;
    }
    if (r != 0) {
        close(fd);
        why = "Wi-Fi: " + ip_ + " 연결 거부 — iPad에서 PadLink 방송을 시작하세요";
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

int Receiver::OpenUsb(std::string& why) {
    std::vector<UsbmuxDevice> devices;
    if (!UsbmuxList(devices, why)) return -1;
    auto it = std::find_if(devices.begin(), devices.end(),
                           [](const UsbmuxDevice& d) { return d.connectionType == "USB"; });
    if (it == devices.end()) {
        why = "USB로 연결된 iPad 없음 — 케이블을 꽂고 iPad에서 '이 컴퓨터 신뢰'를 누르세요";
        return -1;
    }
    int result = -1;
    int fd = UsbmuxConnect(it->id, port_, result, why);
    if (fd < 0 && result == 3) why = "USB: iPad 연결됨 — iPad에서 PadLink 방송을 시작하세요";
    return fd;
}

void Receiver::ReadLoop(int fd) {
    std::vector<uint8_t> payload;
    for (;;) {
        wire::Header h{};
        if (!RecvAll(fd, &h, sizeof(h))) return;
        if (h.length > (64u << 20)) {
            Log(Format("잘못된 프레임 길이 %u, 연결을 다시 맺음", h.length));
            return;
        }
        payload.resize(h.length);
        if (h.length > 0 && !RecvAll(fd, payload.data(), h.length)) return;
        bytes += sizeof(h) + h.length;

        switch (h.kind) {
        case wire::kHello:
            Log("방송: iPad 인사 " + std::string(payload.begin(), payload.end()));
            break;
        case wire::kVideoFrame:
            ++videoFrames;
            if (cb_.onVideo) cb_.onVideo(std::move(payload), (h.flags & 1) != 0, uint8_t(h.flags >> 4), h.pts);
            payload = {};
            break;
        case wire::kAudioPCM:
            if (h.length >= 8 && payload[4] == 2 && payload[5] == 16 && cb_.onAudio) {
                uint32_t rate = 0;
                std::memcpy(&rate, payload.data(), 4);
                cb_.onAudio(reinterpret_cast<const int16_t*>(payload.data() + 8), (h.length - 8) / 4, int(rate));
            }
            break;
        case wire::kLog:
            Log("[iPad] " + std::string(payload.begin(), payload.end()));
            break;
        default:
            break;
        }
    }
}
