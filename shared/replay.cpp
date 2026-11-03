#include "replay.h"

#include <algorithm>
#include <climits>

namespace {
constexpr int64_t kSecond = 1000000;

// 키프레임 앞부분의 SPS(7)·PPS(8)를 시작 코드째로 뽑는다
std::vector<uint8_t> ExtractParameterSets(const std::vector<uint8_t>& au) {
    std::vector<uint8_t> out;
    const size_t n = au.size();
    auto nextStart = [&](size_t from) {
        for (size_t k = from; k + 3 <= n; ++k)
            if (au[k] == 0 && au[k + 1] == 0 && au[k + 2] == 1) return k;
        return n;
    };
    size_t start = nextStart(0);
    while (start < n) {
        size_t payload = start + 3;
        size_t next = nextStart(payload);
        size_t end = next;
        while (end > payload && au[end - 1] == 0) --end;  // 다음 4바이트 시작 코드의 앞 0
        if (payload < end) {
            uint8_t type = au[payload] & 0x1F;
            if (type == 7 || type == 8) {
                const uint8_t code[4] = {0, 0, 0, 1};
                out.insert(out.end(), code, code + 4);
                out.insert(out.end(), au.begin() + long(payload), au.begin() + long(end));
            }
        }
        start = next;
    }
    return out;
}
}  // namespace

void ReplayBuffer::SetSeconds(int seconds) {
    std::lock_guard lock(mutex_);
    seconds_ = std::clamp(seconds, 5, 600);
}

int ReplayBuffer::Seconds() const {
    std::lock_guard lock(mutex_);
    return seconds_;
}

void ReplayBuffer::Clear() {
    std::lock_guard lock(mutex_);
    ClearLocked();
}

void ReplayBuffer::ClearLocked() {
    gops_.clear();
    audio_.clear();
    bytes_ = 0;
    havePts_ = false;
}

void ReplayBuffer::AddVideo(const uint8_t* data, size_t size, bool key, uint8_t orientation, uint64_t ptsUs,
                            int64_t nowUs) {
    std::lock_guard lock(mutex_);
    int64_t pts = int64_t(ptsUs);
    // 방송을 새로 시작하면 iPad 시각이 이어지지 않는다 → 처음부터 다시 모은다
    if (havePts_ && (pts < lastPts_ || pts - lastPts_ > 10 * kSecond)) ClearLocked();
    if (key) gops_.emplace_back();
    if (gops_.empty()) return;  // 첫 키프레임 전에는 풀 수 없다
    havePts_ = true;
    lastPts_ = pts;
    Frame f;
    f.data.assign(data, data + size);
    f.key = key;
    f.orientation = orientation;
    f.pts = pts;
    f.arrival = nowUs;
    bytes_ += size;
    gops_.back().push_back(std::move(f));
    PruneLocked(nowUs);
}

void ReplayBuffer::AddAudio(const int16_t* pcm, size_t frames, int rate, int64_t nowUs) {
    if (frames == 0 || rate <= 0) return;
    std::lock_guard lock(mutex_);
    Chunk c;
    c.pcm.assign(pcm, pcm + frames * 2);
    c.rate = rate;
    c.arrival = nowUs;
    bytes_ += frames * 4;
    audio_.push_back(std::move(c));
    PruneLocked(nowUs);
}

// 마지막 N초를 풀려면 '그 시점 이전의 마지막 키프레임'부터 있어야 한다
void ReplayBuffer::PruneLocked(int64_t nowUs) {
    const int64_t cutoff = nowUs - int64_t(seconds_) * kSecond;
    while (gops_.size() >= 2 && gops_[1].front().arrival <= cutoff) {
        for (const Frame& f : gops_.front()) bytes_ -= f.data.size();
        gops_.pop_front();
    }
    const int64_t keepFrom = cutoff - kSecond;
    while (!audio_.empty() && audio_.front().arrival < keepFrom) {
        bytes_ -= audio_.front().pcm.size() * 2;
        audio_.pop_front();
    }
}

ReplayBuffer::Status ReplayBuffer::GetStatus(int64_t nowUs) const {
    std::lock_guard lock(mutex_);
    Status s;
    s.bytes = bytes_;
    if (!gops_.empty()) {
        int64_t first = gops_.front().front().arrival;
        s.seconds = std::min<double>(double(nowUs - first) / kSecond, seconds_);
    }
    return s;
}

bool ReplayBuffer::Snapshot(ReplayClip& clip, std::string& error) const {
    std::vector<Frame> frames;
    std::vector<Chunk> chunks;
    int seconds = 0;
    {
        std::lock_guard lock(mutex_);
        seconds = seconds_;
        if (gops_.empty()) {
            error = "아직 받은 화면이 없음";
            return false;
        }
        int64_t newest = gops_.back().back().arrival;
        if (!audio_.empty()) newest = std::max(newest, audio_.back().arrival);
        const int64_t cutoff = newest - int64_t(seconds) * kSecond;
        // 자르는 시점 이전의 마지막 키프레임 묶음부터
        size_t start = 0;
        for (size_t g = 0; g < gops_.size(); ++g)
            if (gops_[g].front().arrival <= cutoff) start = g;
        for (size_t g = start; g < gops_.size(); ++g)
            for (const Frame& f : gops_[g]) frames.push_back(f);
        for (const Chunk& c : audio_) chunks.push_back(c);
    }

    // iPad 영상 시각 → PC 시각: 가장 덜 늦게 도착한 프레임을 기준으로 맞춘다 (네트워크 흔들림 제거)
    int64_t offset = INT64_MAX;
    for (const Frame& f : frames) offset = std::min(offset, f.arrival - f.pts);
    int64_t newest = frames.back().arrival;
    if (!chunks.empty()) newest = std::max(newest, chunks.back().arrival);
    const int64_t cutoff = newest - int64_t(seconds) * kSecond;
    const int64_t firstTime = frames.front().pts + offset;
    const int64_t clipStart = std::max(cutoff, firstTime);

    clip = ReplayClip{};
    clip.parameterSets = ExtractParameterSets(frames.front().data);
    if (clip.parameterSets.empty()) {
        error = "첫 키프레임에 SPS/PPS가 없음";
        return false;
    }
    // 자르는 시점 이전 프레임들은 디코딩에만 필요하므로 클립 맨 앞에 1ms씩 몰아 둔다
    int64_t prev = -1000;
    for (const Frame& f : frames) {
        int64_t t = std::max<int64_t>(f.pts + offset - clipStart, 0);
        t = std::max(t, prev + 1000);
        clip.frames.push_back(f.data);
        clip.timesUs.push_back(t);
        clip.keys.push_back(f.key ? 1 : 0);
        clip.videoBytes += f.data.size();
        prev = t;
    }
    clip.orientation = frames.back().orientation;
    clip.durationUs = std::max(newest - clipStart, prev + 16667);

    // 소리: 끊김 없이 이어진다고 보고 시작 시각을 추정한다. 중간에 0.15초 넘게 비면 무음으로 채운다.
    if (!chunks.empty()) {
        const int rate = chunks.back().rate;
        std::vector<const Chunk*> same;
        for (const Chunk& c : chunks)
            if (c.rate == rate) same.push_back(&c);
        int64_t cum = 0, audioOrigin = INT64_MAX;
        for (const Chunk* c : same) {
            cum += int64_t(c->pcm.size() / 2);
            audioOrigin = std::min(audioOrigin, c->arrival - cum * kSecond / rate);
        }
        std::vector<int16_t> pcm;
        cum = 0;
        for (const Chunk* c : same) {
            int64_t count = int64_t(c->pcm.size() / 2);
            int64_t expectedEnd = audioOrigin + (cum + count) * kSecond / rate;
            int64_t late = c->arrival - expectedEnd;
            if (late > 150000) {
                int64_t silence = late * rate / kSecond;
                pcm.insert(pcm.end(), size_t(silence * 2), 0);
                cum += silence;
            }
            pcm.insert(pcm.end(), c->pcm.begin(), c->pcm.end());
            cum += count;
        }
        // 클립 시작에 맞춰 앞을 자르거나, 늦게 시작하면 그만큼 뒤로 민다
        int64_t skip = (clipStart - audioOrigin) * rate / kSecond;
        if (skip > 0) {
            size_t drop = std::min(pcm.size(), size_t(skip) * 2);
            pcm.erase(pcm.begin(), pcm.begin() + long(drop));
            clip.audioStartUs = 0;
        } else {
            clip.audioStartUs = -skip * kSecond / rate;
        }
        int64_t maxFrames = std::max<int64_t>(0, (clip.durationUs - clip.audioStartUs) * rate / kSecond);
        if (int64_t(pcm.size() / 2) > maxFrames) pcm.resize(size_t(maxFrames) * 2);
        if (!pcm.empty()) {
            clip.audioRate = rate;
            clip.audio = std::move(pcm);
        }
    }
    return true;
}
