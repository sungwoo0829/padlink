#include "replay_writer.h"

#include <knownfolders.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {
ComPtr<IMFSample> MakeSample(const void* data, size_t size, int64_t timeUs, int64_t durationUs, bool key) {
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(DWORD(size), &buffer))) return nullptr;
    BYTE* p = nullptr;
    if (FAILED(buffer->Lock(&p, nullptr, nullptr))) return nullptr;
    std::memcpy(p, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(DWORD(size));
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(LONGLONG(timeUs) * 10);
    sample->SetSampleDuration(LONGLONG(std::max<int64_t>(durationUs, 1)) * 10);
    if (key) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    return sample;
}
}  // namespace

std::wstring ReplayFolder() {
    std::wstring base;
    PWSTR videos = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &videos)) && videos) base = videos;
    CoTaskMemFree(videos);
    if (base.empty()) base = ExeDir();
    std::wstring dir = base + L"\\PadLink";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool WriteReplayMp4(const ReplayClip& clip, int turns, UINT width, UINT height, const std::wstring& path,
                    std::wstring& error) {
    if (clip.frames.empty() || width == 0 || height == 0) {
        error = L"저장할 영상이 없음";
        return false;
    }
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 2);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    ComPtr<IMFSinkWriter> writer;
    HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), &writer);
    if (FAILED(hr)) {
        error = Format(L"MP4 파일을 만들 수 없음 0x%08X", (unsigned)hr);
        return false;
    }

    // 영상: 받은 H.264를 그대로 넣는다 (입력 형식 = 출력 형식 → 인코더가 끼지 않음)
    ComPtr<IMFMediaType> video;
    MFCreateMediaType(&video);
    video->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    video->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(video.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(video.Get(), MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(video.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    video->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    double seconds = std::max(clip.durationUs / 1e6, 0.1);
    video->SetUINT32(MF_MT_AVG_BITRATE, UINT32(std::min(clip.videoBytes * 8 / seconds, 4.0e9)));
    video->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, clip.parameterSets.data(), UINT32(clip.parameterSets.size()));
    // MF 회전은 반시계 방향: 시계 방향 90도 = 270
    video->SetUINT32(MF_MT_VIDEO_ROTATION, UINT32((360 - 90 * (turns & 3)) % 360));
    DWORD videoStream = 0;
    hr = writer->AddStream(video.Get(), &videoStream);
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(videoStream, video.Get(), nullptr);
    if (FAILED(hr)) {
        error = Format(L"영상 트랙을 만들 수 없음 0x%08X", (unsigned)hr);
        return false;
    }

    // 소리: PCM → AAC 192kbps (MF의 AAC 인코더는 44.1k·48k만)
    DWORD audioStream = 0;
    bool audio = clip.audioRate == 44100 || clip.audioRate == 48000;
    if (audio && !clip.audio.empty()) {
        ComPtr<IMFMediaType> aac, pcm;
        MFCreateMediaType(&aac);
        aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(clip.audioRate));
        aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
        MFCreateMediaType(&pcm);
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(clip.audioRate));
        pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, UINT32(clip.audioRate) * 4);
        hr = writer->AddStream(aac.Get(), &audioStream);
        if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(audioStream, pcm.Get(), nullptr);
        if (FAILED(hr)) audio = false;  // 소리 없이라도 영상은 남긴다
    } else {
        audio = false;
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        error = Format(L"MP4 쓰기를 시작할 수 없음 0x%08X", (unsigned)hr);
        return false;
    }

    // 시간 순서대로 영상과 소리를 번갈아 넣는다
    const int64_t audioFrames = audio ? int64_t(clip.audio.size() / 2) : 0;
    const int64_t chunk = 1024;
    int64_t audioPos = 0;
    size_t vi = 0;
    while (vi < clip.frames.size() || audioPos < audioFrames) {
        int64_t tv = vi < clip.frames.size() ? clip.timesUs[vi] : INT64_MAX;
        int64_t ta = audioPos < audioFrames ? clip.audioStartUs + audioPos * 1000000 / clip.audioRate : INT64_MAX;
        if (tv <= ta) {
            int64_t next = vi + 1 < clip.frames.size() ? clip.timesUs[vi + 1] : clip.durationUs;
            auto sample = MakeSample(clip.frames[vi].data(), clip.frames[vi].size(), tv, next - tv, clip.keys[vi] != 0);
            hr = sample ? writer->WriteSample(videoStream, sample.Get()) : E_OUTOFMEMORY;
            ++vi;
        } else {
            int64_t n = std::min(chunk, audioFrames - audioPos);
            auto sample = MakeSample(clip.audio.data() + audioPos * 2, size_t(n) * 4, ta, n * 1000000 / clip.audioRate,
                                     false);
            hr = sample ? writer->WriteSample(audioStream, sample.Get()) : E_OUTOFMEMORY;
            audioPos += n;
        }
        if (FAILED(hr)) {
            error = Format(L"영상·소리를 쓰다가 실패 0x%08X", (unsigned)hr);
            return false;
        }
    }
    hr = writer->Finalize();
    if (FAILED(hr)) {
        error = Format(L"MP4 마무리 실패 0x%08X", (unsigned)hr);
        return false;
    }
    return true;
}
