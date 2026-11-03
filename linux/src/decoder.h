#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

// iPad 송출 영상(H.264 Annex-B)을 FFmpeg로 푼다. 화면에는 항상 가장 최근 프레임만 쓴다.
class Decoder {
public:
    ~Decoder() { Stop(); }
    bool Start();
    void Stop();
    void Push(std::vector<uint8_t>&& annexB, bool key, uint8_t orientation);
    // 새 프레임이 있으면 소유권을 넘긴다 (없으면 nullptr). 호출한 쪽이 av_frame_free.
    AVFrame* TakeLatest(uint8_t& orientation);

    std::atomic<uint64_t> decoded{0};

private:
    struct Packet {
        std::vector<uint8_t> data;
        bool key = false;
        uint8_t orientation = 1;
    };
    void Thread();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Packet> queue_;
    bool stop_ = false;
    bool waitKey_ = true;

    std::mutex latestMutex_;
    AVFrame* latest_ = nullptr;
    uint8_t latestOrientation_ = 1;
};
