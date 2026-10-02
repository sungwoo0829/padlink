#include "net.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "log.h"
#include "usbmux.h"

void Receiver::Start(LinkMode mode, std::wstring ip, uint16_t port, std::wstring label, std::wstring waitHint,
                     ReceiverCallbacks callbacks) {
    Stop();
    mode_ = mode;
    ip_ = std::move(ip);
    port_ = port;
    label_ = std::move(label);
    waitHint_ = std::move(waitHint);
    cb_ = std::move(callbacks);
    stop_ = false;
    thread_ = std::thread([this] { Loop(); });
}

void Receiver::Stop() {
    stop_ = true;
    {
        std::lock_guard lock(socketMutex_);
        if (socket_ != INVALID_SOCKET) shutdown(socket_, SD_BOTH);
    }
    waitCv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool Receiver::Send(uint8_t kind, const void* data, size_t len, uint8_t flags) {
    std::lock_guard sendLock(sendMutex_);
    SOCKET s = INVALID_SOCKET;
    {
        std::lock_guard lock(socketMutex_);
        s = socket_;
    }
    if (s == INVALID_SOCKET) return false;
    wire::Header h{kind, flags, 0, uint32_t(len), 0};
    return SendAll(s, &h, sizeof(h)) && (len == 0 || SendAll(s, data, len));
}

void Receiver::Loop() {
    std::wstring lastWhy;
    while (!stop_) {
        std::wstring why;
        SOCKET s = Open(why);
        if (s == INVALID_SOCKET) {
            if (why != lastWhy) {
                Log(label_ + L": " + why);
                cb_.onStatus(why);
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
                closesocket(s);
                break;
            }
            socket_ = s;
        }
        BOOL noDelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        int rcvbuf = 4 << 20;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

        std::wstring how = mode_ == LinkMode::Usb ? L"USB" : L"Wi-Fi";
        connected = true;
        Log(label_ + L": " + how + L"로 iPad에 연결됨");
        cb_.onStatus(how + L" 연결됨");
        if (cb_.onConnected) cb_.onConnected();
        ReadLoop(s);
        connected = false;
        if (cb_.onDisconnected) cb_.onDisconnected();
        {
            std::lock_guard lock(socketMutex_);
            socket_ = INVALID_SOCKET;
        }
        // 다른 스레드가 보내는 중이면 끝날 때까지 기다린 뒤 닫는다
        shutdown(s, SD_BOTH);
        { std::lock_guard sendLock(sendMutex_); }
        closesocket(s);
        if (!stop_) {
            Log(label_ + L": 연결 끊김, 다시 연결 중");
            cb_.onStatus(L"연결 끊김, 다시 연결 중…");
        }
    }
}

SOCKET Receiver::Open(std::wstring& why) { return mode_ == LinkMode::Usb ? OpenUsb(why) : OpenWifi(why); }

SOCKET Receiver::OpenWifi(std::wstring& why) {
    addrinfoW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfoW* result = nullptr;
    std::wstring port = std::to_wstring(port_);
    if (GetAddrInfoW(ip_.c_str(), port.c_str(), &hints, &result) != 0 || !result) {
        why = L"IP 주소를 해석할 수 없음: " + ip_;
        return INVALID_SOCKET;
    }
    SOCKET s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (s == INVALID_SOCKET) {
        FreeAddrInfoW(result);
        why = L"소켓 생성 실패";
        return INVALID_SOCKET;
    }
    // 2초 안에 안 붙으면 포기 (방송이 꺼져 있으면 바로 거부됨)
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    connect(s, result->ai_addr, (int)result->ai_addrlen);
    FreeAddrInfoW(result);
    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(s, &writable);
    FD_SET(s, &failed);
    timeval timeout{2, 0};
    int ready = select(0, nullptr, &writable, &failed, &timeout);
    if (ready <= 0 || FD_ISSET(s, &failed)) {
        closesocket(s);
        why = ready == 0 ? L"Wi-Fi: " + ip_ + L" 응답 없음 (같은 네트워크인지 확인)"
                         : L"Wi-Fi: " + ip_ + L" 연결 거부 — " + waitHint_;
        return INVALID_SOCKET;
    }
    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    return s;
}

SOCKET Receiver::OpenUsb(std::wstring& why) {
    std::vector<UsbmuxDevice> devices;
    if (!UsbmuxList(devices, why)) return INVALID_SOCKET;
    auto it = std::find_if(devices.begin(), devices.end(),
                           [](const UsbmuxDevice& d) { return d.connectionType == "USB"; });
    if (it == devices.end()) {
        why = L"USB로 연결된 iPad 없음 — 케이블을 꽂고 iPad에서 '이 컴퓨터 신뢰'를 누르세요";
        return INVALID_SOCKET;
    }
    int result = -1;
    SOCKET s = UsbmuxConnect(it->id, port_, result, why);
    if (s == INVALID_SOCKET && result == 3) why = L"USB: iPad 연결됨 — " + waitHint_;
    return s;
}

void Receiver::ReadLoop(SOCKET s) {
    std::vector<uint8_t> payload;
    for (;;) {
        wire::Header h{};
        if (!RecvAll(s, &h, sizeof(h))) return;
        if (h.length > (64u << 20)) {
            Log(Format(L"잘못된 프레임 길이 %u, 연결을 다시 맺음", h.length));
            return;
        }
        payload.resize(h.length);
        if (h.length > 0 && !RecvAll(s, payload.data(), h.length)) return;
        bytes += sizeof(h) + h.length;

        switch (h.kind) {
        case wire::kHello:
            Log(label_ + L": iPad 인사 " + Widen(std::string(payload.begin(), payload.end())));
            break;
        case wire::kPenSamples:
            if (cb_.onPenSamples) cb_.onPenSamples(payload.data(), payload.size());
            break;
        case wire::kPenButton:
            if (h.length >= 2 && cb_.onPenButton) cb_.onPenButton(payload[0], payload[1]);
            break;
        case wire::kKeyframeRequest:
            if (cb_.onKeyframeRequest) cb_.onKeyframeRequest();
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
                size_t frames = (h.length - 8) / 4;
                cb_.onAudio(reinterpret_cast<const int16_t*>(payload.data() + 8), frames, (int)rate);
            }
            break;
        case wire::kLog:
            Log(L"[iPad " + label_ + L"] " + Widen(std::string(payload.begin(), payload.end())));
            break;
        default:
            break;
        }
    }
}
