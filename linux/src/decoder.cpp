#include "decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}

#include <cstring>

#include "log.h"

bool Decoder::Start() {
    if (!avcodec_find_decoder(AV_CODEC_ID_H264)) {
        Log("FFmpeg에 H.264 디코더가 없음");
        return false;
    }
    stop_ = false;
    thread_ = std::thread([this] { Thread(); });
    return true;
}

void Decoder::Stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(latestMutex_);
    av_frame_free(&latest_);
}

void Decoder::Push(std::vector<uint8_t>&& annexB, bool key, uint8_t orientation) {
    {
        std::lock_guard lock(mutex_);
        if (queue_.size() > 20) {
            // 디코딩이 밀리면 쌓인 걸 버리고 다음 키프레임부터
            queue_.clear();
            waitKey_ = true;
        }
        queue_.push_back(Packet{std::move(annexB), key, orientation});
    }
    cv_.notify_one();
}

AVFrame* Decoder::TakeLatest(uint8_t& orientation) {
    std::lock_guard lock(latestMutex_);
    AVFrame* f = latest_;
    latest_ = nullptr;
    orientation = latestOrientation_;
    return f;
}

void Decoder::Thread() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    // 지연 없이: 프레임 스레드(한 프레임씩 늦어짐) 대신 슬라이스 스레드
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->thread_type = FF_THREAD_SLICE;
    ctx->thread_count = 0;
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        Log("H.264 디코더를 열 수 없음");
        avcodec_free_context(&ctx);
        return;
    }
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool loggedSize = false;

    for (;;) {
        Packet p;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) break;
            p = std::move(queue_.front());
            queue_.pop_front();
            if (waitKey_) {
                if (!p.key) continue;
                waitKey_ = false;
            }
        }
        if (av_new_packet(packet, int(p.data.size())) < 0) continue;
        std::memcpy(packet->data, p.data.data(), p.data.size());
        if (p.key) packet->flags |= AV_PKT_FLAG_KEY;
        int r = avcodec_send_packet(ctx, packet);
        av_packet_unref(packet);
        if (r < 0) {
            Log(Format("영상 디코딩 실패 (%d), 다음 키프레임부터 다시", r));
            std::lock_guard lock(mutex_);
            waitKey_ = true;
            continue;
        }
        while (avcodec_receive_frame(ctx, frame) == 0) {
            ++decoded;
            if (!loggedSize) {
                loggedSize = true;
                Log(Format("영상 %dx%d (%s, %s)", frame->width, frame->height,
                           av_get_pix_fmt_name(AVPixelFormat(frame->format)),
                           frame->color_range == AVCOL_RANGE_JPEG ? "0-255" : "16-235"));
            }
            AVFrame* copy = av_frame_clone(frame);
            av_frame_unref(frame);
            std::lock_guard lock(latestMutex_);
            av_frame_free(&latest_);
            latest_ = copy;
            latestOrientation_ = p.orientation;
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
}
