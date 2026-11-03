#include "snapshot.h"

#include <SDL3/SDL.h>

extern "C" {
#include <libswscale/swscale.h>
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#pragma GCC diagnostic pop

bool FrameToRgba(const AVFrame* frame, int turns, bool fullRange, std::vector<uint8_t>& rgba, int& width,
                 int& height) {
    if (!frame || frame->width <= 0 || frame->height <= 0) return false;
    const int w = frame->width, h = frame->height;
    SwsContext* sws = sws_getContext(w, h, AVPixelFormat(frame->format), w, h, AV_PIX_FMT_RGBA, SWS_POINT, nullptr,
                                     nullptr, nullptr);
    if (!sws) return false;
    // iPad 영상은 BT.709. 범위는 프레임 정보(또는 사용자가 바꾼 값)를 따른다.
    const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(sws, coefficients, fullRange ? 1 : 0, coefficients, 1, 0, 1 << 16, 1 << 16);
    std::vector<uint8_t> upright(size_t(w) * size_t(h) * 4);
    uint8_t* dst[4] = {upright.data(), nullptr, nullptr, nullptr};
    int dstStride[4] = {w * 4, 0, 0, 0};
    sws_scale(sws, frame->data, frame->linesize, 0, h, dst, dstStride);
    sws_freeContext(sws);

    turns &= 3;
    width = turns & 1 ? h : w;
    height = turns & 1 ? w : h;
    if (turns == 0) {
        rgba = std::move(upright);
        return true;
    }
    rgba.assign(size_t(width) * size_t(height) * 4, 0);
    const uint32_t* src = reinterpret_cast<const uint32_t*>(upright.data());
    uint32_t* out = reinterpret_cast<uint32_t*>(rgba.data());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int nx = 0, ny = 0;
            switch (turns) {
            case 1: nx = h - 1 - y; ny = x; break;          // 시계 방향 90도
            case 2: nx = w - 1 - x; ny = h - 1 - y; break;  // 180도
            default: nx = y; ny = w - 1 - x; break;         // 270도
            }
            out[size_t(ny) * size_t(width) + size_t(nx)] = src[size_t(y) * size_t(w) + size_t(x)];
        }
    }
    return true;
}

namespace {
void AppendBytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    out->insert(out->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}

const void* ClipboardData(void* userdata, const char* mimeType, size_t* size) {
    auto* png = static_cast<std::vector<uint8_t>*>(userdata);
    if (!mimeType || SDL_strcmp(mimeType, "image/png") != 0) {
        *size = 0;
        return nullptr;
    }
    *size = png->size();
    return png->data();
}

void ClipboardCleanup(void* userdata) { delete static_cast<std::vector<uint8_t>*>(userdata); }
}  // namespace

bool CopyPngToClipboard(const std::vector<uint8_t>& rgba, int width, int height, std::string& error) {
    if (width <= 0 || height <= 0 || rgba.size() < size_t(width) * size_t(height) * 4) {
        error = "복사할 화면이 아직 없음";
        return false;
    }
    auto* png = new std::vector<uint8_t>();
    png->reserve(rgba.size() / 2);
    stbi_write_png_compression_level = 4;  // 2.5MP를 빠르게
    if (!stbi_write_png_to_func(AppendBytes, png, width, height, 4, rgba.data(), width * 4)) {
        delete png;
        error = "PNG로 바꾸지 못함";
        return false;
    }
    const char* mimeTypes[] = {"image/png"};
    if (!SDL_SetClipboardData(ClipboardData, ClipboardCleanup, png, mimeTypes, 1)) {
        error = std::string("클립보드에 넣지 못함 — ") + SDL_GetError();
        return false;  // png는 SDL이 들고 있다가 다음 복사·종료 때 cleanup으로 지운다
    }
    return true;
}
