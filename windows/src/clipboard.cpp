#include "clipboard.h"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {
bool EncodePng(const std::vector<uint8_t>& bgra, int width, int height, std::vector<uint8_t>& out) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(UINT(width), UINT(height))))
        return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA) return false;
    UINT stride = UINT(width) * 4;
    if (FAILED(frame->WritePixels(UINT(height), stride, UINT(bgra.size()), const_cast<BYTE*>(bgra.data()))) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return false;

    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return false;
    HGLOBAL memory = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &memory))) return false;
    const void* p = GlobalLock(memory);
    if (!p) return false;
    out.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + stat.cbSize.LowPart);
    GlobalUnlock(memory);
    return true;
}

HGLOBAL CopyToGlobal(const void* data, size_t size) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!h) return nullptr;
    void* p = GlobalLock(h);
    std::memcpy(p, data, size);
    GlobalUnlock(h);
    return h;
}

// 아래→위 행 순서의 32비트 BI_RGB 비트맵
HGLOBAL MakeDib(const std::vector<uint8_t>& bgra, int width, int height) {
    const size_t rowBytes = size_t(width) * 4;
    const size_t imageBytes = rowBytes * size_t(height);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + imageBytes);
    if (!h) return nullptr;
    auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(h));
    *header = BITMAPINFOHEADER{};
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = width;
    header->biHeight = height;
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = DWORD(imageBytes);
    auto* pixels = reinterpret_cast<uint8_t*>(header + 1);
    for (int y = 0; y < height; ++y)
        std::memcpy(pixels + size_t(height - 1 - y) * rowBytes, bgra.data() + size_t(y) * rowBytes, rowBytes);
    GlobalUnlock(h);
    return h;
}
}  // namespace

bool CopyImageToClipboard(HWND owner, const std::vector<uint8_t>& bgra, int width, int height, std::wstring& error) {
    if (width <= 0 || height <= 0 || bgra.size() < size_t(width) * size_t(height) * 4) {
        error = L"복사할 화면이 아직 없음";
        return false;
    }
    HGLOBAL dib = MakeDib(bgra, width, height);
    std::vector<uint8_t> png;
    HGLOBAL pngMemory = EncodePng(bgra, width, height, png) ? CopyToGlobal(png.data(), png.size()) : nullptr;
    if (!dib && !pngMemory) {
        error = L"메모리를 잡지 못함";
        return false;
    }

    // 다른 프로그램이 클립보드를 잡고 있으면 잠깐 기다린다
    bool opened = false;
    for (int attempt = 0; attempt < 10 && !(opened = OpenClipboard(owner) != FALSE); ++attempt) Sleep(20);
    if (!opened) {
        if (dib) GlobalFree(dib);
        if (pngMemory) GlobalFree(pngMemory);
        error = L"클립보드를 열 수 없음 (다른 프로그램이 쓰는 중)";
        return false;
    }
    EmptyClipboard();
    if (pngMemory && !SetClipboardData(RegisterClipboardFormatW(L"PNG"), pngMemory)) GlobalFree(pngMemory);
    if (dib && !SetClipboardData(CF_DIB, dib)) GlobalFree(dib);
    CloseClipboard();
    return true;
}
