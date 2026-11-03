#include "replay_writer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
}

#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {
constexpr AVRational kMicro{1, 1000000};

std::string AvError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

struct AudioEncoder {
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    ~AudioEncoder() {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
    }
};
}  // namespace

std::string ReplayFolder() {
    const char* home = std::getenv("HOME");
    std::string base = home ? home : ".";
    std::string videos = base + "/Videos";
    // ~/.config/user-dirs.dirs의 XDG_VIDEOS_DIR="$HOME/비디오"
    std::ifstream dirs(base + "/.config/user-dirs.dirs");
    for (std::string line; std::getline(dirs, line);) {
        if (line.rfind("XDG_VIDEOS_DIR=", 0) != 0) continue;
        std::string v = line.substr(15);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (v.rfind("$HOME", 0) == 0) v = base + v.substr(5);
        if (!v.empty()) videos = v;
    }
    mkdir(videos.c_str(), 0755);
    std::string dir = videos + "/PadLink";
    mkdir(dir.c_str(), 0755);
    return dir;
}

bool WriteReplayMp4(const ReplayClip& clip, int turns, int width, int height, const std::string& path,
                    std::string& error) {
    if (clip.frames.empty() || width <= 0 || height <= 0) {
        error = "저장할 영상이 없음";
        return false;
    }
    AVFormatContext* oc = nullptr;
    int r = avformat_alloc_output_context2(&oc, nullptr, "mp4", path.c_str());
    if (r < 0 || !oc) {
        error = "MP4를 만들 수 없음: " + AvError(r);
        return false;
    }
    auto fail = [&](const std::string& message) {
        error = message;
        if (oc->pb) avio_closep(&oc->pb);
        avformat_free_context(oc);
        return false;
    };

    // 영상: 받은 H.264 그대로. 회전은 표시 행렬로 남긴다 (FFmpeg 각도는 반시계 방향)
    AVStream* vs = avformat_new_stream(oc, nullptr);
    vs->time_base = AVRational{1, 90000};
    AVCodecParameters* vp = vs->codecpar;
    vp->codec_type = AVMEDIA_TYPE_VIDEO;
    vp->codec_id = AV_CODEC_ID_H264;
    vp->width = width;
    vp->height = height;
    vp->extradata = static_cast<uint8_t*>(av_mallocz(clip.parameterSets.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(vp->extradata, clip.parameterSets.data(), clip.parameterSets.size());
    vp->extradata_size = int(clip.parameterSets.size());
    if (turns & 3) {
        AVPacketSideData* sd = av_packet_side_data_new(&vp->coded_side_data, &vp->nb_coded_side_data,
                                                       AV_PKT_DATA_DISPLAYMATRIX, sizeof(int32_t) * 9, 0);
        if (sd) av_display_rotation_set(reinterpret_cast<int32_t*>(sd->data), -90.0 * (turns & 3));
    }

    // 소리: PCM → AAC
    AudioEncoder ae;
    AVStream* as = nullptr;
    if (clip.audioRate > 0 && !clip.audio.empty()) {
        const AVCodec* aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (aac) {
            ae.ctx = avcodec_alloc_context3(aac);
            ae.ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
            ae.ctx->sample_rate = clip.audioRate;
            av_channel_layout_default(&ae.ctx->ch_layout, 2);
            ae.ctx->bit_rate = 192000;
            ae.ctx->time_base = AVRational{1, clip.audioRate};
            if (oc->oformat->flags & AVFMT_GLOBALHEADER) ae.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(ae.ctx, aac, nullptr) == 0) {
                as = avformat_new_stream(oc, nullptr);
                as->time_base = AVRational{1, clip.audioRate};
                avcodec_parameters_from_context(as->codecpar, ae.ctx);
                ae.frame = av_frame_alloc();
                ae.frame->format = ae.ctx->sample_fmt;
                ae.frame->sample_rate = ae.ctx->sample_rate;
                ae.frame->nb_samples = ae.ctx->frame_size > 0 ? ae.ctx->frame_size : 1024;
                av_channel_layout_copy(&ae.frame->ch_layout, &ae.ctx->ch_layout);
                av_frame_get_buffer(ae.frame, 0);
                ae.packet = av_packet_alloc();
            }
        }
    }

    if (!(oc->oformat->flags & AVFMT_NOFILE) && (r = avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE)) < 0)
        return fail("파일을 열 수 없음: " + AvError(r));
    AVDictionary* options = nullptr;
    av_dict_set(&options, "movflags", "+faststart", 0);  // 디스코드 등에서 바로 재생되게 앞부분에 목차
    r = avformat_write_header(oc, &options);
    av_dict_free(&options);
    if (r < 0) return fail("MP4 머리말을 쓸 수 없음: " + AvError(r));

    AVPacket* pkt = av_packet_alloc();
    auto writeAudioPackets = [&]() -> bool {
        while (avcodec_receive_packet(ae.ctx, ae.packet) == 0) {
            av_packet_rescale_ts(ae.packet, ae.ctx->time_base, as->time_base);
            ae.packet->stream_index = as->index;
            if (av_interleaved_write_frame(oc, ae.packet) < 0) return false;
        }
        return true;
    };

    // 시간 순서대로 영상과 소리를 번갈아 넣는다
    const int64_t audioFrames = as ? int64_t(clip.audio.size() / 2) : 0;
    const int frameSize = as ? ae.frame->nb_samples : 0;
    const int64_t audioStartSample = as ? clip.audioStartUs * clip.audioRate / 1000000 : 0;
    int64_t audioPos = 0;
    size_t vi = 0;
    bool ok = true;
    while (ok && (vi < clip.frames.size() || (as && audioPos < audioFrames))) {
        int64_t tv = vi < clip.frames.size() ? clip.timesUs[vi] : INT64_MAX;
        int64_t ta = as && audioPos < audioFrames ? clip.audioStartUs + audioPos * 1000000 / clip.audioRate : INT64_MAX;
        if (tv <= ta) {
            const auto& data = clip.frames[vi];
            av_new_packet(pkt, int(data.size()));
            std::memcpy(pkt->data, data.data(), data.size());
            pkt->pts = pkt->dts = av_rescale_q(tv, kMicro, vs->time_base);
            int64_t next = vi + 1 < clip.frames.size() ? clip.timesUs[vi + 1] : clip.durationUs;
            pkt->duration = std::max<int64_t>(1, av_rescale_q(next - tv, kMicro, vs->time_base));
            if (clip.keys[vi]) pkt->flags |= AV_PKT_FLAG_KEY;
            pkt->stream_index = vs->index;
            ok = av_interleaved_write_frame(oc, pkt) >= 0;
            ++vi;
        } else {
            av_frame_make_writable(ae.frame);
            float* left = reinterpret_cast<float*>(ae.frame->data[0]);
            float* right = reinterpret_cast<float*>(ae.frame->data[1]);
            for (int i = 0; i < frameSize; ++i) {
                int64_t s = audioPos + i;
                left[i] = s < audioFrames ? clip.audio[size_t(s) * 2] / 32768.0f : 0.0f;
                right[i] = s < audioFrames ? clip.audio[size_t(s) * 2 + 1] / 32768.0f : 0.0f;
            }
            ae.frame->pts = audioStartSample + audioPos;
            ok = avcodec_send_frame(ae.ctx, ae.frame) >= 0 && writeAudioPackets();
            audioPos += frameSize;
        }
    }
    if (ok && as) ok = avcodec_send_frame(ae.ctx, nullptr) >= 0 && writeAudioPackets();
    av_packet_free(&pkt);
    if (!ok) return fail("영상·소리를 쓰다가 실패");
    r = av_write_trailer(oc);
    avio_closep(&oc->pb);
    avformat_free_context(oc);
    if (r < 0) {
        error = "MP4 마무리 실패: " + AvError(r);
        return false;
    }
    return true;
}
