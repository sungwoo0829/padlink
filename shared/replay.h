#pragma once
// 리플레이 하이라이트: 받은 송출(H.264·PCM)을 다시 인코딩하지 않고 마지막 N초만 메모리에 들고 있다가
// 저장할 때 그 구간을 잘라 준다. Windows·Linux 수신 앱이 같이 쓴다.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

struct ReplayClip {
    std::vector<std::vector<uint8_t>> frames;  // H.264 Annex-B. 첫 프레임은 키프레임
    std::vector<int64_t> timesUs;              // 클립 시작 기준 표시 시각 (계속 늘어남)
    std::vector<uint8_t> keys;                 // 1 = 키프레임
    int64_t durationUs = 0;
    uint8_t orientation = 1;                   // 마지막 프레임의 iPad 방향
    std::vector<uint8_t> parameterSets;        // SPS·PPS (Annex-B)
    int audioRate = 0;                         // 0이면 소리 없음
    std::vector<int16_t> audio;                // 스테레오 인터리브
    int64_t audioStartUs = 0;                  // 소리가 시작되는 클립 시각 (0 이상)
    size_t videoBytes = 0;
};

class ReplayBuffer {
public:
    struct Status {
        double seconds = 0;  // 지금 저장하면 담길 길이
        size_t bytes = 0;    // 들고 있는 메모리
    };

    void SetSeconds(int seconds);
    int Seconds() const;
    void Clear();
    // nowUs: PC 단조 시계(마이크로초). ptsUs: iPad가 붙인 영상 시각
    void AddVideo(const uint8_t* data, size_t size, bool key, uint8_t orientation, uint64_t ptsUs, int64_t nowUs);
    void AddAudio(const int16_t* pcm, size_t frames, int rate, int64_t nowUs);
    bool Snapshot(ReplayClip& clip, std::string& error) const;
    Status GetStatus(int64_t nowUs) const;

private:
    struct Frame {
        std::vector<uint8_t> data;
        bool key = false;
        uint8_t orientation = 1;
        int64_t pts = 0;
        int64_t arrival = 0;
    };
    struct Chunk {
        std::vector<int16_t> pcm;
        int rate = 0;
        int64_t arrival = 0;
    };
    void PruneLocked(int64_t nowUs);
    void ClearLocked();

    mutable std::mutex mutex_;
    std::deque<std::deque<Frame>> gops_;  // 키프레임마다 한 묶음
    std::deque<Chunk> audio_;
    size_t bytes_ = 0;
    int seconds_ = 30;
    int64_t lastPts_ = 0;
    bool havePts_ = false;
};
