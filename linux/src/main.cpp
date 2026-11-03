// PadLink 수신 (리눅스): iPad 송출(화면·소리)을 USB/Wi-Fi로 받아 창에 띄우고 오디오 인터페이스로 재생한다.
// 디스코드에서는 'PadLink 화면' 창을 공유하면 소리까지 같이 나간다.
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "audio.h"
#include "decoder.h"
#include "log.h"
#include "net.h"
#include "settings.h"
#include "snapshot.h"
#include "wire.h"

namespace {
constexpr const char* kFontPaths[] = {
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",  // fonts-noto-cjk (우분투 기본)
    "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
};

enum class Action { None, Copy, Borderless, Fit, Rotate, Range, Hide };

struct App {
    Settings settings;
    Receiver receiver;
    Decoder decoder;
    AudioOut audio;

    SDL_Window* mainWindow = nullptr;
    SDL_Renderer* mainRenderer = nullptr;
    ImGuiContext* mainCtx = nullptr;
    SDL_Window* videoWindow = nullptr;
    SDL_Renderer* videoRenderer = nullptr;
    ImGuiContext* videoCtx = nullptr;

    SDL_Texture* texture = nullptr;
    int texW = 0, texH = 0;
    bool texFull = true;
    AVFrame* frame = nullptr;  // 지금 보이는 프레임 (복사·다시 그리기용)
    uint8_t orientation = 1;
    bool warnedFormat = false;

    bool running = true;
    bool videoDirty = true;
    bool openMenu = false;
    std::atomic<bool> menuOpen{false};
    Action pending = Action::None;

    std::mutex statusMutex;
    std::string linkStatus = "연결 안 함";
    std::atomic<int> bitrateMbps{20};
    std::vector<std::string> devices;
    char ipBuf[64] = {};

    Uint64 lastStatTick = 0;
    uint64_t lastBytes = 0, lastFrames = 0;
    double fps = 0, mbps = 0;
    int statTicks = 0;
    uint64_t logVersion = 0;
};

App* g_app = nullptr;

// CGImagePropertyOrientation(iPad) → 시계 방향 90도 회전 횟수
int OrientationTurns(uint8_t orientation) {
    switch (orientation) {
    case 3: return 2;
    case 6: return 1;
    case 8: return 3;
    default: return 0;
    }
}

int Turns(const App& a) { return (OrientationTurns(a.orientation) + a.settings.rotation) & 3; }

bool FrameFullRange(const AVFrame* f) {
    return f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
}

// ---- 영상 ----

void UpdateTexture(App& a) {
    AVFrame* f = a.frame;
    if (!f) return;
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
        if (!a.warnedFormat) {
            a.warnedFormat = true;
            Log(Format("지원하지 않는 영상 형식 %d", f->format));
        }
        return;
    }
    bool full = FrameFullRange(f) != (a.settings.rangeFlip != 0);
    if (!a.texture || a.texW != f->width || a.texH != f->height || a.texFull != full) {
        if (a.texture) SDL_DestroyTexture(a.texture);
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_IYUV);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, f->width);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, f->height);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                              full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED);
        a.texture = SDL_CreateTextureWithProperties(a.videoRenderer, props);
        SDL_DestroyProperties(props);
        if (!a.texture) {
            Log(std::string("화면 텍스처를 만들 수 없음 — ") + SDL_GetError());
            return;
        }
        a.texW = f->width;
        a.texH = f->height;
        a.texFull = full;
    }
    SDL_UpdateYUVTexture(a.texture, nullptr, f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2],
                         f->linesize[2]);
    a.videoDirty = true;
}

void CopyFrame(App& a) {
    if (!a.frame) {
        Log("복사할 화면이 아직 없음");
        return;
    }
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    bool full = FrameFullRange(a.frame) != (a.settings.rangeFlip != 0);
    if (!FrameToRgba(a.frame, Turns(a), full, rgba, w, h)) {
        Log("화면을 그림으로 바꾸지 못함");
        return;
    }
    std::string error;
    if (CopyPngToClipboard(rgba, w, h, error)) Log(Format("화면을 클립보드에 복사함 (%dx%d, PNG)", w, h));
    else Log("복사 실패: " + error);
}

// 높이는 두고 너비를 영상 비율에 맞춘다 → 검은 여백이 없어진다. 화면을 넘으면 줄인다.
void FitToAspect(App& a) {
    if (!a.texW || !a.texH) {
        Log("아직 받은 화면이 없어 비율을 맞출 수 없음");
        return;
    }
    int turns = Turns(a);
    int vw = turns & 1 ? a.texH : a.texW, vh = turns & 1 ? a.texW : a.texH;
    int w = 0, h = 0;
    SDL_GetWindowSize(a.videoWindow, &w, &h);
    int nh = h, nw = h * vw / vh;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(a.videoWindow), &usable)) {
        if (nw > usable.w) {
            nw = usable.w;
            nh = nw * vh / vw;
        }
        if (nh > usable.h) {
            nh = usable.h;
            nw = nh * vw / vh;
        }
    }
    SDL_SetWindowSize(a.videoWindow, nw, nh);
    a.settings.videoW = nw;
    a.settings.videoH = nh;
    SaveSettings(a.settings);
}

// 테두리가 없을 때: 가장자리는 크기 조절, 나머지는 끌어서 옮기기 (메뉴가 떠 있으면 클릭 그대로)
SDL_HitTestResult SDLCALL HitTest(SDL_Window* window, const SDL_Point* p, void*) {
    if (g_app && g_app->menuOpen) return SDL_HITTEST_NORMAL;
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    const int b = 8;
    bool left = p->x < b, right = p->x >= w - b, top = p->y < b, bottom = p->y >= h - b;
    if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (left) return SDL_HITTEST_RESIZE_LEFT;
    if (right) return SDL_HITTEST_RESIZE_RIGHT;
    if (top) return SDL_HITTEST_RESIZE_TOP;
    if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
    return SDL_HITTEST_DRAGGABLE;
}

// 테두리 없이: 스크린샷·디스코드 창 공유에 제목 표시줄과 테두리가 안 들어간다. 켤 때 검은 여백도 없앤다.
void SetBorderless(App& a, bool on) {
    a.settings.borderless = on ? 1 : 0;
    SDL_SetWindowBordered(a.videoWindow, !on);
    SDL_SetWindowHitTest(a.videoWindow, on ? HitTest : nullptr, nullptr);
    if (on) FitToAspect(a);
    SaveSettings(a.settings);
    a.videoDirty = true;
}

void RunAction(App& a, Action action) {
    switch (action) {
    case Action::Copy: CopyFrame(a); break;
    case Action::Borderless: SetBorderless(a, a.settings.borderless == 0); break;
    case Action::Fit: FitToAspect(a); break;
    case Action::Rotate:
        a.settings.rotation = (a.settings.rotation + 1) & 3;
        SaveSettings(a.settings);
        a.videoDirty = true;
        break;
    case Action::Range:
        a.settings.rangeFlip ^= 1;
        SaveSettings(a.settings);
        UpdateTexture(a);
        break;
    case Action::Hide: SDL_HideWindow(a.videoWindow); break;
    case Action::None: break;
    }
}

void RenderVideo(App& a) {
    SDL_Renderer* r = a.videoRenderer;
    SDL_SetRenderScale(r, 1.0f, 1.0f);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    if (a.texture) {
        int ow = 0, oh = 0;
        SDL_GetCurrentRenderOutputSize(r, &ow, &oh);
        int turns = Turns(a);
        float cw = float(turns & 1 ? a.texH : a.texW), ch = float(turns & 1 ? a.texW : a.texH);
        float s = std::min(ow / cw, oh / ch);
        float dw = a.texW * s, dh = a.texH * s;
        // 회전은 dst 가운데를 기준으로 하므로 회전 전 크기로 가운데에 놓는다
        SDL_FRect dst{ow / 2.0f - dw / 2.0f, oh / 2.0f - dh / 2.0f, dw, dh};
        SDL_RenderTextureRotated(r, a.texture, nullptr, &dst, turns * 90.0, nullptr, SDL_FLIP_NONE);
    }

    // 우클릭 메뉴
    ImGui::SetCurrentContext(a.videoCtx);
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (a.openMenu) {
        ImGui::OpenPopup("video-menu");
        a.openMenu = false;
    }
    bool open = false;
    if (ImGui::BeginPopup("video-menu")) {
        open = true;
        if (ImGui::MenuItem("복사하기", "Ctrl+C")) a.pending = Action::Copy;
        ImGui::Separator();
        if (ImGui::MenuItem("테두리 없이", "B", a.settings.borderless != 0)) a.pending = Action::Borderless;
        if (ImGui::MenuItem("영상 비율에 맞추기 (검은 여백 없애기)", "F")) a.pending = Action::Fit;
        ImGui::Separator();
        if (ImGui::MenuItem("90° 회전", "R")) a.pending = Action::Rotate;
        if (ImGui::MenuItem("색 범위 바꾸기", "C")) a.pending = Action::Range;
        if (ImGui::MenuItem("창 숨기기")) a.pending = Action::Hide;
        ImGui::EndPopup();
    }
    a.menuOpen = open;
    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(r, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), r);
    SDL_RenderPresent(r);
}

// ---- 연결 ----

void SendBroadcastConfig(App& a) {
    std::string json = "{\"bitrate\":" + std::to_string(a.bitrateMbps.load() * 1000000) + "}";
    a.receiver.Send(wire::kControl, json.data(), json.size());
}

void SetLinkStatus(App& a, const std::string& s) {
    std::lock_guard lock(a.statusMutex);
    a.linkStatus = s;
}

void ToggleConnect(App& a) {
    if (a.receiver.Running()) {
        a.receiver.Stop();
        SetLinkStatus(a, "연결 안 함");
        Log("연결 중지");
        return;
    }
    a.settings.ip = a.ipBuf;
    SaveSettings(a.settings);
    ReceiverCallbacks cb;
    cb.onVideo = [&a](std::vector<uint8_t>&& data, bool key, uint8_t orientation) {
        a.decoder.Push(std::move(data), key, orientation);
    };
    cb.onAudio = [&a](const int16_t* pcm, size_t frames, int rate) { a.audio.Push(pcm, frames, rate); };
    cb.onConnected = [&a] { SendBroadcastConfig(a); };
    cb.onStatus = [&a](const std::string& s) { SetLinkStatus(a, s); };
    a.receiver.Start(a.settings.mode == 1 ? LinkMode::Wifi : LinkMode::Usb, a.settings.ip, wire::kPort, cb);
    SetLinkStatus(a, "연결 중…");
    SDL_ShowWindow(a.videoWindow);
}

// ---- 설정 창 ----

void RenderControl(App& a) {
    Uint64 now = SDL_GetTicks();
    if (now - a.lastStatTick >= 1000) {
        double dt = a.lastStatTick ? (now - a.lastStatTick) / 1000.0 : 0;
        uint64_t bytes = a.receiver.bytes.load(), frames = a.receiver.videoFrames.load();
        a.fps = dt > 0 ? (frames - a.lastFrames) / dt : 0;
        a.mbps = dt > 0 ? (bytes - a.lastBytes) * 8 / dt / 1e6 : 0;
        a.lastBytes = bytes;
        a.lastFrames = frames;
        a.lastStatTick = now;
        // 원격으로 상태를 볼 수 있게 연결 중에는 1분마다 로그에도 남긴다
        if (a.receiver.connected && ++a.statTicks % 60 == 0) {
            AudioStats s = a.audio.Stats();
            Log(Format("상태: 영상 %dx%d %.0ffps %.1fMbps | 소리 %dHz 버퍼 %.0fms 보정 %+.2f%% 모자람 %u 건너뜀 %u",
                       a.texW, a.texH, a.fps, a.mbps, s.sourceRate, s.bufferedMs, s.correctionPct, s.drained,
                       s.skips));
        }
    }

    ImGui::SetCurrentContext(a.mainCtx);
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // 연결
    bool running = a.receiver.Running();
    ImGui::BeginDisabled(running);
    ImGui::RadioButton("USB", &a.settings.mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Wi-Fi", &a.settings.mode, 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    ImGui::InputText("##ip", a.ipBuf, sizeof(a.ipBuf));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(running ? "끊기" : "연결", ImVec2(ImGui::GetFontSize() * 5, 0))) ToggleConnect(a);

    std::string link;
    {
        std::lock_guard lock(a.statusMutex);
        link = a.linkStatus;
    }
    if (a.receiver.connected)
        ImGui::Text("%s · 영상 %dx%d %.0ffps %.1fMbps", link.c_str(), a.texW, a.texH, a.fps, a.mbps);
    else
        ImGui::TextWrapped("%s", link.c_str());
    AudioStats s = a.audio.Stats();
    if (s.active)
        ImGui::Text("소리: %s · iPad %dHz · 버퍼 %.0fms · 보정 %+.2f%% · 모자람 %u · 건너뜀 %u", s.device.c_str(),
                    s.sourceRate, s.bufferedMs, s.correctionPct, s.drained, s.skips);
    else
        ImGui::Text("소리: 꺼짐 (로그 참고)");

    // 소리
    ImGui::SeparatorText("소리 — 오디오 인터페이스로 듣기 (디스코드도 이 소리를 잡아요)");
    const char* preview = a.settings.audioDevice.empty() ? "기본 출력" : a.settings.audioDevice.c_str();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22);
    if (ImGui::BeginCombo("출력 장치", preview)) {
        if (ImGui::Selectable("기본 출력", a.settings.audioDevice.empty())) {
            a.settings.audioDevice.clear();
            a.audio.Open(a.settings.audioDevice, a.settings.bufferMs, a.settings.volume);
            SaveSettings(a.settings);
        }
        for (const auto& name : a.devices) {
            if (ImGui::Selectable(name.c_str(), name == a.settings.audioDevice)) {
                a.settings.audioDevice = name;
                a.audio.Open(a.settings.audioDevice, a.settings.bufferMs, a.settings.volume);
                SaveSettings(a.settings);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("새로고침")) a.devices = a.audio.Devices();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::SliderInt("버퍼 ms (끊기면 늘리기)", &a.settings.bufferMs, 10, 300)) a.audio.SetBuffer(a.settings.bufferMs);
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings(a.settings);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::SliderInt("볼륨 %", &a.settings.volume, 0, 200)) a.audio.SetVolume(a.settings.volume);
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings(a.settings);

    // 송출 화질
    ImGui::SeparatorText("송출 화질 (iPad 인코더에 바로 적용)");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::InputInt("Mbps", &a.settings.broadcastBitrateMbps)) {
        a.settings.broadcastBitrateMbps = std::clamp(a.settings.broadcastBitrateMbps, 2, 100);
    }
    ImGui::SameLine();
    if (ImGui::Button("적용")) {
        a.bitrateMbps = a.settings.broadcastBitrateMbps;
        SaveSettings(a.settings);
        if (a.receiver.connected) {
            SendBroadcastConfig(a);
            Log(Format("송출 화질 %dMbps로 바꿈", a.settings.broadcastBitrateMbps));
        }
    }

    // 화면 창
    ImGui::SeparatorText("'PadLink 화면' 창");
    if (ImGui::Button("창 보이기")) {
        SDL_ShowWindow(a.videoWindow);
        SDL_RaiseWindow(a.videoWindow);
    }
    ImGui::SameLine();
    if (ImGui::Button(a.settings.borderless ? "테두리 보이기" : "테두리 없이")) RunAction(a, Action::Borderless);
    ImGui::SameLine();
    if (ImGui::Button("영상 비율에 맞추기")) RunAction(a, Action::Fit);
    ImGui::SameLine();
    if (ImGui::Button("복사하기")) RunAction(a, Action::Copy);
    ImGui::SameLine();
    if (ImGui::Button("90° 회전")) RunAction(a, Action::Rotate);
    ImGui::SameLine();
    if (ImGui::Button("색 범위")) RunAction(a, Action::Range);
    ImGui::TextWrapped(
        "디스코드에서는 'PadLink 화면' 창을 공유하세요 (공식 클라이언트, 소리 포함). 창에서 우클릭하면 같은 메뉴가 "
        "나와요. 단축키: Ctrl+C 복사(원본 해상도 PNG), B 테두리, F 비율 맞추기, R 회전, C 색 범위. 테두리가 없을 "
        "때는 아무 데나 끌어서 옮기고 가장자리로 크기를 바꿔요.");

    // 로그
    ImGui::SeparatorText("로그");
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (const auto& line : LogLines()) ImGui::TextUnformatted(line.c_str());
    uint64_t version = LogVersion();
    if (version != a.logVersion) {
        a.logVersion = version;
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();

    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(a.mainRenderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(a.mainRenderer, 30, 32, 36, 255);
    SDL_RenderClear(a.mainRenderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), a.mainRenderer);
    SDL_RenderPresent(a.mainRenderer);
}

ImGuiContext* CreateImGui(SDL_Window* window, SDL_Renderer* renderer, float scale) {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    bool loaded = false;
    for (const char* path : kFontPaths) {
        if (access(path, R_OK) != 0) continue;
        ImFontConfig cfg;
        // Noto Sans CJK 묶음 파일: 0 = JP, 1 = KR
        cfg.FontNo = std::strstr(path, ".ttc") ? 1 : 0;
        if (io.Fonts->AddFontFromFileTTF(path, 17.0f, &cfg)) {
            loaded = true;
            break;
        }
    }
    if (!loaded) Log("한글 폰트를 찾지 못함 — 'sudo apt install fonts-noto-cjk'");
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    return ctx;
}

void DestroyImGui(ImGuiContext* ctx) {
    ImGui::SetCurrentContext(ctx);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
}

void HandleEvent(App& a, const SDL_Event& e) {
    // 각 ImGui 문맥은 자기 창의 이벤트만 받는다
    ImGui::SetCurrentContext(a.mainCtx);
    ImGui_ImplSDL3_ProcessEvent(&e);
    ImGui::SetCurrentContext(a.videoCtx);
    ImGui_ImplSDL3_ProcessEvent(&e);

    const SDL_WindowID mainId = SDL_GetWindowID(a.mainWindow), videoId = SDL_GetWindowID(a.videoWindow);
    switch (e.type) {
    case SDL_EVENT_QUIT:
        a.running = false;
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (e.window.windowID == mainId) a.running = false;
        else if (e.window.windowID == videoId) SDL_HideWindow(a.videoWindow);  // 닫으면 디스코드 공유가 끊기니 숨기기만
        break;
    case SDL_EVENT_WINDOW_RESIZED:
        if (e.window.windowID == videoId) {
            a.settings.videoW = e.window.data1;
            a.settings.videoH = e.window.data2;
            a.videoDirty = true;
        }
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_SHOWN:
        if (e.window.windowID == videoId) a.videoDirty = true;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (e.key.windowID != videoId || a.menuOpen) break;
        switch (e.key.key) {
        case SDLK_C: RunAction(a, (e.key.mod & SDL_KMOD_CTRL) ? Action::Copy : Action::Range); break;
        case SDLK_B: RunAction(a, Action::Borderless); break;
        case SDLK_F: RunAction(a, Action::Fit); break;
        case SDLK_R: RunAction(a, Action::Rotate); break;
        default: break;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.windowID != videoId) break;
        if (e.button.button == SDL_BUTTON_RIGHT) {
            a.openMenu = true;
        } else if (e.button.button == SDL_BUTTON_LEFT && e.button.clicks == 2 && !a.menuOpen) {
            RunAction(a, Action::Borderless);
        }
        break;
    default:
        break;
    }
}
}  // namespace

int main(int, char**) {
    signal(SIGPIPE, SIG_IGN);
    std::string dir = ConfigDir();
    LogInit(dir + "/padlink-recv.log");
    Log("PadLink 수신 (리눅스) 시작 — 설정·로그: " + dir);

    App a;
    g_app = &a;
    a.settings = LoadSettings();
    a.bitrateMbps = a.settings.broadcastBitrateMbps;
    std::strncpy(a.ipBuf, a.settings.ip.c_str(), sizeof(a.ipBuf) - 1);

    SDL_SetAppMetadata("PadLink 수신", "0.1", "io.github.sungwoo0829.padlink.recv");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "PadLink iPad 소리");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        Log(std::string("SDL 초기화 실패 — ") + SDL_GetError());
        return 1;
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1.0f;

    a.mainWindow = SDL_CreateWindow("PadLink 수신", int(780 * scale), int(700 * scale),
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    a.mainRenderer = SDL_CreateRenderer(a.mainWindow, nullptr);
    int vw = a.settings.videoW >= 100 ? a.settings.videoW : int(960 * scale);
    int vh = a.settings.videoH >= 100 ? a.settings.videoH : int(667 * scale);
    SDL_WindowFlags videoFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (a.settings.borderless) videoFlags |= SDL_WINDOW_BORDERLESS;
    a.videoWindow = SDL_CreateWindow("PadLink 화면", vw, vh, videoFlags);
    a.videoRenderer = SDL_CreateRenderer(a.videoWindow, nullptr);
    if (!a.mainWindow || !a.mainRenderer || !a.videoWindow || !a.videoRenderer) {
        Log(std::string("창을 만들 수 없음 — ") + SDL_GetError());
        return 1;
    }
    if (a.settings.borderless) SDL_SetWindowHitTest(a.videoWindow, HitTest, nullptr);
    Log(Format("화면: %s / %s · 소리: %s", SDL_GetCurrentVideoDriver(), SDL_GetRendererName(a.videoRenderer),
               SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "없음"));

    a.mainCtx = CreateImGui(a.mainWindow, a.mainRenderer, scale);
    a.videoCtx = CreateImGui(a.videoWindow, a.videoRenderer, scale);

    a.decoder.Start();
    a.devices = a.audio.Devices();
    a.audio.Open(a.settings.audioDevice, a.settings.bufferMs, a.settings.volume);

    Uint64 lastUi = 0;
    while (a.running) {
        SDL_Event e;
        bool have = SDL_WaitEventTimeout(&e, 4);
        while (have) {
            HandleEvent(a, e);
            have = SDL_PollEvent(&e);
        }
        uint8_t orientation = 1;
        if (AVFrame* f = a.decoder.TakeLatest(orientation)) {
            av_frame_free(&a.frame);
            a.frame = f;
            if (orientation != a.orientation) a.videoDirty = true;
            a.orientation = orientation;
            UpdateTexture(a);
        }
        a.audio.Tick();

        bool videoVisible = (SDL_GetWindowFlags(a.videoWindow) & SDL_WINDOW_HIDDEN) == 0;
        if (videoVisible && (a.videoDirty || a.menuOpen || a.openMenu)) {
            a.videoDirty = false;
            RenderVideo(a);
            Action action = a.pending;
            a.pending = Action::None;
            RunAction(a, action);
        }
        Uint64 now = SDL_GetTicks();
        if (now - lastUi >= 33) {
            lastUi = now;
            RenderControl(a);
        }
    }

    a.receiver.Stop();
    a.decoder.Stop();
    a.audio.Close();
    a.settings.ip = a.ipBuf;
    SaveSettings(a.settings);
    av_frame_free(&a.frame);
    DestroyImGui(a.videoCtx);
    DestroyImGui(a.mainCtx);
    if (a.texture) SDL_DestroyTexture(a.texture);
    SDL_DestroyRenderer(a.videoRenderer);
    SDL_DestroyRenderer(a.mainRenderer);
    SDL_DestroyWindow(a.videoWindow);
    SDL_DestroyWindow(a.mainWindow);
    SDL_Quit();
    Log("종료");
    return 0;
}
