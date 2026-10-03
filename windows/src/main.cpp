#include "common.h"

#include <commctrl.h>

#include <algorithm>
#include <memory>
#include <thread>
#include <objbase.h>

#include "audio.h"
#include "log.h"
#include "net.h"
#include "pen.h"
#include "screen.h"
#include "settings.h"
#include "vdd.h"
#include "video.h"

namespace {
constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_STATUS = WM_APP + 2;
constexpr UINT WM_APP_PEN_LINK = WM_APP + 3;  // wParam 1 = 펜 연결됨, 0 = 끊김
constexpr UINT WM_APP_VDD_DONE = WM_APP + 4;  // wParam 1 = 켬, lParam = new std::wstring* 오류(없으면 nullptr)
constexpr UINT_PTR kStatsTimer = 1;

enum ControlId : int {
    ID_USB = 101,
    ID_WIFI,
    ID_IP,
    ID_CONNECT,
    ID_STATUS,
    ID_ASIO,
    ID_ASIO_BUF,
    ID_ASIO_VOL,
    ID_DISC,
    ID_DISC_BUF,
    ID_DISC_VOL,
    ID_PEN_MON,
    ID_DISPLAY,
    ID_APPLY,
    ID_SHOW_VIDEO,
    ID_ROTATE,
    ID_LOG,
    ID_DISP_BR,
    ID_CAST_BR,
};

HINSTANCE g_instance;
HWND g_main;
HWND g_video;
HFONT g_font;
float g_scale = 1.0f;

Settings g_settings;
Receiver g_receiver;  // 화면 방송 (47800)
Receiver g_penLink;   // 펜 모드 (47810)
VideoPipeline g_videoPipe;
AudioEngine g_audio;
PenInjector g_pen;
ScreenSender g_screen;
uint64_t g_lastScreenBytes = 0;
uint64_t g_lastScreenFrames = 0;
bool g_videoOk = false;
bool g_penOk = false;
std::vector<MonitorInfo> g_monitors;
std::vector<std::wstring> g_comboDevices;  // 펜 모니터 콤보 항목별 값 (장치 이름 또는 kVddChoice)
constexpr wchar_t kVddChoice[] = L"VDD";   // 가상 모니터를 필요할 때만 켜는 자동 모드
VddState g_vdd;
bool g_vddBusy = false;           // 켜고 끄는 중 (작업 스레드)
ULONGLONG g_vddUnneededSince = 0;
ULONGLONG g_vddLastToggle = 0;
ULONGLONG g_lastMonitorRefresh = 0;
std::wstring g_screenDevice;  // 화면 보내기가 지금 캡처 중인 모니터
std::wstring g_linkStatus = L"연결 안 함";
std::wstring g_penStatus = L"연결 안 함";
uint64_t g_lastPenSamples = 0;
uint64_t g_lastBytes = 0;
uint64_t g_lastFrames = 0;
ULONGLONG g_lastTick = 0;
std::wstring g_lastStatus;
int g_statusTicks = 0;

int S(int v) { return int(v * g_scale + 0.5f); }

HWND Control(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
             DWORD exStyle = 0) {
    HWND hwnd = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_main,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return hwnd;
}

HWND Item(int id) { return GetDlgItem(g_main, id); }

std::wstring ItemText(int id) {
    wchar_t buf[512];
    GetWindowTextW(Item(id), buf, _countof(buf));
    return buf;
}

int ItemInt(int id, int fallback) {
    BOOL ok = FALSE;
    UINT v = GetDlgItemInt(g_main, id, &ok, FALSE);
    return ok ? int(v) : fallback;
}

void AppendLog(const std::wstring& line) {
    HWND edit = Item(ID_LOG);
    int len = GetWindowTextLengthW(edit);
    if (len > 200000) {
        SendMessageW(edit, EM_SETSEL, 0, len / 2);
        SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
        len = GetWindowTextLengthW(edit);
    }
    std::wstring text = line + L"\r\n";
    SendMessageW(edit, EM_SETSEL, len, len);
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

// 콤보 0번은 "(사용 안 함)" 또는 "(첫 번째 드라이버)"
void FillCombo(int id, const std::vector<std::wstring>& names, const std::wstring& selected, const wchar_t* none) {
    HWND combo = Item(id);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(none));
    int pick = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(names[i].c_str()));
        if (names[i] == selected) pick = int(i) + 1;
    }
    SendMessageW(combo, CB_SETCURSEL, pick, 0);
}

std::wstring ComboValue(int id) {
    HWND combo = Item(id);
    int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel <= 0) return L"";
    int len = (int)SendMessageW(combo, CB_GETLBTEXTLEN, sel, 0);
    std::wstring text(size_t(len) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(text.data()));
    text.resize(size_t(len));
    return text;
}

// 디스코드용 기본값: 내가 듣지 않을 가능성이 높은 장치 (가상 케이블 > 디지털 출력)
std::wstring GuessDiscordDevice(const std::vector<std::wstring>& names) {
    for (const wchar_t* hint : {L"CABLE Input", L"Digital Output", L"디지털 출력", L"S/PDIF", L"SPDIF"})
        for (const auto& n : names)
            if (n.find(hint) != std::wstring::npos) return n;
    return L"";
}

void ReadUi() {
    g_settings.mode = SendMessageW(Item(ID_WIFI), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
    g_settings.ip = ItemText(ID_IP);
    if (g_audio.Loaded()) {
        g_settings.asioDriver = ComboValue(ID_ASIO);
        g_settings.discordDevice = ComboValue(ID_DISC);
    }
    g_settings.asioBufferMs = ItemInt(ID_ASIO_BUF, g_settings.asioBufferMs);
    g_settings.asioVolume = ItemInt(ID_ASIO_VOL, g_settings.asioVolume);
    g_settings.discordBufferMs = ItemInt(ID_DISC_BUF, g_settings.discordBufferMs);
    g_settings.discordVolume = ItemInt(ID_DISC_VOL, g_settings.discordVolume);
    g_settings.displayBitrateMbps = std::clamp(ItemInt(ID_DISP_BR, g_settings.displayBitrateMbps), 5, 200);
    g_settings.broadcastBitrateMbps = std::clamp(ItemInt(ID_CAST_BR, g_settings.broadcastBitrateMbps), 2, 100);
}

std::wstring MonitorLabel(const MonitorInfo& m) {
    std::wstring name = m.device;
    if (name.rfind(L"\\\\.\\", 0) == 0) name = name.substr(4);
    return Format(L"%ls · %ldx%ld%ls", name.c_str(), m.rect.right - m.rect.left, m.rect.bottom - m.rect.top,
                  m.primary ? L" · 주 모니터" : L"");
}

const MonitorInfo* CurrentVdd() {
    for (const auto& m : g_monitors)
        if (m.vdd) return &m;
    return nullptr;
}

const MonitorInfo* PrimaryMonitor() {
    for (const auto& m : g_monitors)
        if (m.primary) return &m;
    return g_monitors.empty() ? nullptr : &g_monitors.front();
}

// 펜이 움직이고 화면을 보낼 모니터. VDD 자동 모드인데 아직 안 켜졌으면 nullptr.
const MonitorInfo* ChosenMonitor() {
    if (g_settings.penMonitor == kVddChoice) return CurrentVdd();
    for (const auto& m : g_monitors)
        if (m.device == g_settings.penMonitor) return &m;
    return PrimaryMonitor();
}

void SetVddOwned(bool owned) {
    if ((g_settings.vddOwned != 0) == owned) return;
    g_settings.vddOwned = owned ? 1 : 0;
    SaveSettings(g_settings);
}

void FillMonitors() {
    g_monitors = ListMonitors();
    g_vdd = VddQuery();
    for (auto& m : g_monitors) m.vdd = g_vdd.active && m.device == g_vdd.device;
    // 예전에 VDD 모니터를 이름으로 골라 뒀으면 자동 모드로 바꾼다 (켤 때마다 이름이 바뀔 수 있음)
    for (const auto& m : g_monitors)
        if (m.vdd && m.device == g_settings.penMonitor) g_settings.penMonitor = kVddChoice;
    g_lastMonitorRefresh = GetTickCount64();

    HWND combo = Item(ID_PEN_MON);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    g_comboDevices.clear();
    if (g_vdd.installed) {
        const MonitorInfo* v = CurrentVdd();
        std::wstring label = v ? Format(L"가상 모니터 (VDD) · %ldx%ld · 펜 모드 동안 켜짐", v->rect.right - v->rect.left,
                                        v->rect.bottom - v->rect.top)
                               : std::wstring(L"가상 모니터 (VDD) · 꺼짐 · 펜 모드를 열면 켜짐");
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        g_comboDevices.push_back(kVddChoice);
    }
    for (const auto& m : g_monitors) {
        if (m.vdd) continue;
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(MonitorLabel(m).c_str()));
        g_comboDevices.push_back(m.device);
    }
    int pick = -1;
    for (size_t i = 0; i < g_comboDevices.size(); ++i)
        if (g_comboDevices[i] == g_settings.penMonitor) pick = int(i);
    if (pick < 0) {
        const MonitorInfo* p = PrimaryMonitor();
        for (size_t i = 0; p && i < g_comboDevices.size(); ++i)
            if (g_comboDevices[i] == p->device) pick = int(i);
    }
    if (pick < 0 && !g_comboDevices.empty()) pick = 0;
    SendMessageW(combo, CB_SETCURSEL, pick, 0);

    // VDD 자동 모드는 화면 보내기가 전제라 체크를 고정한다
    bool vddMode = g_settings.penMonitor == kVddChoice;
    SendMessageW(Item(ID_DISPLAY), BM_SETCHECK, vddMode || g_settings.display ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(Item(ID_DISPLAY), !vddMode);
}

bool DisplayWanted() { return g_settings.display != 0 || g_settings.penMonitor == kVddChoice; }

void SendPenConfig() {
    RECT r = g_pen.Target();
    std::string json = "{\"width\":" + std::to_string(r.right - r.left) + ",\"height\":" +
                       std::to_string(r.bottom - r.top) + "}";
    g_penLink.Send(wire::kPenConfig, json);
}

void ApplyPen() {
    if (!g_penOk) return;
    int sel = (int)SendMessageW(Item(ID_PEN_MON), CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < (int)g_comboDevices.size()) g_settings.penMonitor = g_comboDevices[size_t(sel)];
    PenMapping m;
    // VDD가 켜지기 전에는 주 모니터로 (켜지면 다시 불린다)
    const MonitorInfo* target = ChosenMonitor();
    if (!target) target = PrimaryMonitor();
    m.target = target ? target->rect : RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    m.squeezeKeys = g_settings.squeezeKeys;
    m.doubleTapKeys = g_settings.doubleTapKeys;
    m.pressureGamma = g_settings.pressureGamma;
    m.tilt = g_settings.tilt != 0;
    m.invertTilt = g_settings.invertTilt != 0;
    g_pen.Configure(m);
    if (g_penLink.connected) SendPenConfig();
}

// 펜 채널이 연결돼 있고 '화면 보내기'가 켜져 있으면 펜 모니터를 iPad로 보낸다
void UpdateScreenSender(bool restart) {
    const MonitorInfo* target = ChosenMonitor();
    bool want = DisplayWanted() && g_penLink.connected && target;
    if (g_screen.Running() && (!want || restart || target->device != g_screenDevice)) {
        g_screen.Stop();
        g_screenDevice.clear();
        Log(L"화면 보내기 중지");
    }
    if (!want || g_screen.Running()) return;
    ScreenConfig c;
    c.device = target->device;
    g_screenDevice = c.device;
    c.bitrateMbps = g_settings.displayBitrateMbps;
    c.hevc = g_settings.displayCodec != L"h264";
    // flags: bit0 = 키프레임, bit1 = HEVC
    uint8_t codecFlag = c.hevc ? 2 : 0;
    g_screen.Start(c, [codecFlag](const uint8_t* data, size_t size, bool key) {
        g_penLink.Send(wire::kVideoFrame, data, size, uint8_t((key ? 1 : 0) | codecFlag));
    });
}

void ApplyAudio() {
    if (!g_audio.Loaded()) return;
    AudioConfig c;
    c.asioDriver = g_settings.asioDriver;
    c.discordDevice = g_settings.discordDevice;
    c.asioBufferMs = g_settings.asioBufferMs;
    c.discordBufferMs = g_settings.discordBufferMs;
    c.asioVolume = g_settings.asioVolume;
    c.discordVolume = g_settings.discordVolume;
    c.asioDspBuffer = g_settings.asioDspBuffer;
    g_audio.Apply(c);
}

void SendBroadcastConfig() {
    std::string json = "{\"bitrate\":" + std::to_string(g_settings.broadcastBitrateMbps * 1000000) + "}";
    g_receiver.Send(wire::kControl, json);
}

void ToggleConnect() {
    if (g_receiver.Running()) {
        if (!g_lastStatus.empty()) Log(L"마지막 상태: " + g_lastStatus);
        g_screen.Stop();
        g_receiver.Stop();
        g_penLink.Stop();
        g_pen.Reset();
        SetWindowTextW(Item(ID_CONNECT), L"연결");
        g_linkStatus = L"연결 안 함";
        g_penStatus = L"연결 안 함";
        Log(L"연결 중지");
        return;
    }
    ReadUi();
    SaveSettings(g_settings);
    ReceiverCallbacks cb;
    cb.onVideo = [](std::vector<uint8_t>&& data, bool key, uint8_t orientation, uint64_t pts) {
        if (g_videoOk) g_videoPipe.Push(std::move(data), key, orientation, pts);
    };
    cb.onAudio = [](const int16_t* pcm, size_t frames, int rate) { g_audio.Push(pcm, frames, rate); };
    cb.onConnected = [] { SendBroadcastConfig(); };
    cb.onStatus = [](const std::wstring& status) {
        auto* copy = new std::wstring(status);
        if (!PostMessageW(g_main, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
    };
    LinkMode mode = g_settings.mode == 1 ? LinkMode::Wifi : LinkMode::Usb;
    g_receiver.Start(mode, g_settings.ip, wire::kPort, L"방송", L"iPad에서 PadLink 방송을 시작하세요", cb);

    ReceiverCallbacks pen;
    pen.onPenSamples = [](const uint8_t* data, size_t len) { g_pen.OnSamples(data, len); };
    pen.onPenButton = [](uint8_t button, uint8_t phase) { g_pen.OnButton(button, phase); };
    pen.onKeyframeRequest = [] { g_screen.RequestKeyframe(); };
    pen.onConnected = [] {
        SendPenConfig();
        g_screen.RequestKeyframe();
        PostMessageW(g_main, WM_APP_PEN_LINK, 1, 0);
    };
    pen.onDisconnected = [] {
        g_pen.Reset();
        PostMessageW(g_main, WM_APP_PEN_LINK, 0, 0);
    };
    pen.onStatus = [](const std::wstring& status) {
        auto* copy = new std::wstring(status);
        if (!PostMessageW(g_main, WM_APP_STATUS, 1, reinterpret_cast<LPARAM>(copy))) delete copy;
    };
    if (g_penOk) g_penLink.Start(mode, g_settings.ip, wire::kPenPort, L"펜", L"iPad PadLink 앱에서 펜 모드를 여세요", pen);
    SetWindowTextW(Item(ID_CONNECT), L"끊기");
    g_linkStatus = L"연결 중…";
    g_penStatus = g_penOk ? L"연결 중…" : L"펜 입력을 쓸 수 없음 (로그 참고)";
    ShowWindow(g_video, SW_SHOWNOACTIVATE);
}

std::wstring OutputLine(const wchar_t* label, const AudioOutputStats& s) {
    if (!s.active) return Format(L"%ls: %ls", label, s.error.empty() ? L"꺼짐" : s.error.c_str());
    return Format(L"%ls: %ls · 버퍼 %.0fms · 보정 %+.2f%% · 모자람 %u · 늦음 %u · 건너뜀 %u", label, s.device.c_str(),
                  s.fillMs, s.correctionPct, s.drained, s.late, s.skips);
}

// 가상 모니터(VDD) 자동 모드: 기본은 꺼 두고, iPad 펜 모드가 연결되면 켠다. 펜 모드가 끊기고
// 5초가 지나면(잠깐 끊긴 건 무시) 끈다. 다른 모니터를 골랐으면 PadLink가 켠 게 남았을 때만 끈다.
void VddTick() {
    if (!g_vdd.installed || g_vddBusy) return;
    bool autoMode = g_settings.penMonitor == kVddChoice;
    if (!autoMode && !g_settings.vddOwned) return;
    ULONGLONG now = GetTickCount64();
    bool want = autoMode && g_penLink.connected;
    if (want) g_vddUnneededSince = 0;
    else if (!g_vddUnneededSince) g_vddUnneededSince = now;

    bool turnOn = want && !g_vdd.active;
    bool turnOff = !want && g_vdd.active && now - g_vddUnneededSince > 5000;
    if (!want && !g_vdd.active) SetVddOwned(false);
    if (!turnOn && !turnOff) return;
    if (now - g_vddLastToggle < 3000) return;  // 방금 바꿨으면 화면이 자리 잡을 때까지 기다린다
    g_vddLastToggle = now;
    g_vddBusy = true;
    Log(turnOn ? L"VDD: 가상 모니터 켜는 중" : L"VDD: 쓰지 않아서 가상 모니터 끄는 중");
    // 디스플레이 구성 변경은 화면이 다시 잡힐 때까지 막히므로 작업 스레드에서
    std::thread([turnOn] {
        std::wstring error;
        bool ok = VddSetActive(turnOn, error);
        auto* e = ok ? nullptr : new std::wstring(error);
        if (!PostMessageW(g_main, WM_APP_VDD_DONE, turnOn ? 1 : 0, reinterpret_cast<LPARAM>(e))) delete e;
    }).detach();
}

void UpdateStatus() {
    ULONGLONG now = GetTickCount64();
    double dt = g_lastTick ? (now - g_lastTick) / 1000.0 : 0;
    uint64_t bytes = g_receiver.bytes.load();
    uint64_t frames = g_receiver.videoFrames.load();
    double mbps = dt > 0 ? (bytes - g_lastBytes) * 8 / dt / 1e6 : 0;
    double fps = dt > 0 ? (frames - g_lastFrames) / dt : 0;
    g_lastTick = now;
    g_lastBytes = bytes;
    g_lastFrames = frames;

    std::wstring line1 = g_linkStatus;
    if (g_receiver.connected) {
        line1 += Format(L" · 영상 %dx%d %.0ffps %.1fMbps", g_videoPipe.width.load(), g_videoPipe.height.load(), fps,
                        mbps);
    }
    uint64_t penSamples = g_pen.samples.load();
    double penRate = dt > 0 ? (penSamples - g_lastPenSamples) / dt : 0;
    g_lastPenSamples = penSamples;
    std::wstring penLine = L"펜: " + g_penStatus;
    if (g_penLink.connected) {
        PenStats p = g_pen.Live();
        penLine += Format(L" · %.0f샘플/s · 버튼 %llu회", penRate, g_pen.buttons.load());
        if (p.any)
            penLine += Format(L" · 필압 %.2f · 기울기 X%+d° Y%+d° (세운 각 %.0f°) · 회전 %d°", p.pressure, p.tiltX, p.tiltY,
                              p.altitudeDeg, p.rotation);
    }
    std::wstring screenLine = L"화면 보내기: ";
    uint64_t screenBytes = g_screen.bytes.load(), screenFrames = g_screen.frames.load();
    if (!DisplayWanted()) {
        screenLine += L"꺼짐 (켜면 iPad가 액정타블렛)";
    } else if (!g_screen.Running()) {
        screenLine += g_penLink.connected && g_settings.penMonitor == kVddChoice
                          ? L"가상 모니터(VDD) 켜는 중…"
                          : L"iPad 펜 모드 연결을 기다리는 중";
    } else if (std::wstring e = g_screen.Error(); !e.empty()) {
        screenLine += e;
    } else {
        screenLine += Format(L"%dx%d · %.0ffps · %.1fMbps · %ls", g_screen.width.load(), g_screen.height.load(),
                             dt > 0 ? (screenFrames - g_lastScreenFrames) / dt : 0,
                             dt > 0 ? (screenBytes - g_lastScreenBytes) * 8 / dt / 1e6 : 0,
                             g_settings.displayCodec == L"h264" ? L"H.264" : L"HEVC");
    }
    g_lastScreenBytes = screenBytes;
    g_lastScreenFrames = screenFrames;
    std::wstring line2, line3;
    if (g_audio.Loaded()) {
        AudioStats a = g_audio.Stats();
        if (a.sourceRate > 0) line1 += Format(L" · 소리 %dHz", a.sourceRate);
        line2 = OutputLine(L"ASIO", a.asio);
        line3 = OutputLine(L"디스코드용", a.discord);
    } else {
        line2 = L"소리 꺼짐 — fmod.dll 필요 (로그 참고)";
    }
    SetWindowTextW(Item(ID_STATUS),
                   (line1 + L"\r\n" + penLine + L"\r\n" + screenLine + L"\r\n" + line2 + L"\r\n" + line3).c_str());
    // 원격으로 상태를 볼 수 있게 연결 중에는 1분마다 로그에도 남긴다
    g_lastStatus = line1 + L" | " + penLine + L" | " + screenLine + L" | " + line2 + L" | " + line3;
    if (g_receiver.connected && ++g_statusTicks % 60 == 0) {
        Log(L"상태: " + g_lastStatus);
        PenStats r = g_pen.TakeRange();
        if (r.any)
            Log(Format(L"펜 1분 범위: 필압 최대 %.2f · 기울기 X %+d~%+d° · Y %+d~%+d°", r.maxPressure, r.minTiltX,
                       r.maxTiltX, r.minTiltY, r.maxTiltY));
    }
}

void CreateControls() {
    int y = 12;
    Control(L"STATIC", L"연결", SS_LEFT, 12, y + 4, 40, 20, -1);
    Control(L"BUTTON", L"USB", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 56, y, 64, 26, ID_USB);
    Control(L"BUTTON", L"Wi-Fi", BS_AUTORADIOBUTTON, 124, y, 70, 26, ID_WIFI);
    Control(L"EDIT", g_settings.ip.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 198, y + 1, 150, 24, ID_IP, WS_EX_CLIENTEDGE);
    Control(L"BUTTON", L"연결", BS_PUSHBUTTON | WS_TABSTOP, 358, y, 90, 26, ID_CONNECT);
    SendMessageW(Item(g_settings.mode == 1 ? ID_WIFI : ID_USB), BM_SETCHECK, BST_CHECKED, 0);

    y += 36;
    Control(L"STATIC", L"", SS_LEFT, 12, y, 700, 94, ID_STATUS);

    y += 102;
    Control(L"STATIC", L"내 모니터링 (ASIO)", SS_LEFT, 12, y + 4, 150, 20, -1);
    Control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 166, y, 300, 300, ID_ASIO);
    Control(L"STATIC", L"버퍼ms", SS_RIGHT, 470, y + 4, 56, 20, -1);
    Control(L"EDIT", std::to_wstring(g_settings.asioBufferMs).c_str(), ES_NUMBER | WS_TABSTOP, 530, y + 1, 48, 24,
            ID_ASIO_BUF, WS_EX_CLIENTEDGE);
    Control(L"STATIC", L"볼륨%", SS_RIGHT, 582, y + 4, 52, 20, -1);
    Control(L"EDIT", std::to_wstring(g_settings.asioVolume).c_str(), ES_NUMBER | WS_TABSTOP, 638, y + 1, 48, 24,
            ID_ASIO_VOL, WS_EX_CLIENTEDGE);

    y += 34;
    Control(L"STATIC", L"디스코드용 (WASAPI)", SS_LEFT, 12, y + 4, 150, 20, -1);
    Control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 166, y, 300, 300, ID_DISC);
    Control(L"STATIC", L"버퍼ms", SS_RIGHT, 470, y + 4, 56, 20, -1);
    Control(L"EDIT", std::to_wstring(g_settings.discordBufferMs).c_str(), ES_NUMBER | WS_TABSTOP, 530, y + 1, 48, 24,
            ID_DISC_BUF, WS_EX_CLIENTEDGE);
    Control(L"STATIC", L"볼륨%", SS_RIGHT, 582, y + 4, 52, 20, -1);
    Control(L"EDIT", std::to_wstring(g_settings.discordVolume).c_str(), ES_NUMBER | WS_TABSTOP, 638, y + 1, 48, 24,
            ID_DISC_VOL, WS_EX_CLIENTEDGE);

    y += 34;
    Control(L"STATIC", L"펜 → 모니터", SS_LEFT, 12, y + 4, 150, 20, -1);
    Control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 166, y, 300, 300, ID_PEN_MON);
    Control(L"BUTTON", L"iPad에 화면 보내기 (액정타블렛)", BS_AUTOCHECKBOX | WS_TABSTOP, 474, y, 238, 26, ID_DISPLAY);
    SendMessageW(Item(ID_DISPLAY), BM_SETCHECK, g_settings.display ? BST_CHECKED : BST_UNCHECKED, 0);

    y += 34;
    Control(L"STATIC", L"액정타블렛 화질", SS_LEFT, 12, y + 4, 150, 20, -1);
    Control(L"EDIT", std::to_wstring(g_settings.displayBitrateMbps).c_str(), ES_NUMBER | WS_TABSTOP, 166, y + 1, 48, 24,
            ID_DISP_BR, WS_EX_CLIENTEDGE);
    Control(L"STATIC", L"Mbps · 60fps 고정", SS_LEFT, 220, y + 4, 140, 20, -1);
    Control(L"STATIC", L"송출 화질", SS_RIGHT, 370, y + 4, 100, 20, -1);
    Control(L"EDIT", std::to_wstring(g_settings.broadcastBitrateMbps).c_str(), ES_NUMBER | WS_TABSTOP, 474, y + 1, 48,
            24, ID_CAST_BR, WS_EX_CLIENTEDGE);
    Control(L"STATIC", L"Mbps (적용을 누르면 바로 바뀜)", SS_LEFT, 528, y + 4, 184, 20, -1);

    y += 40;
    Control(L"STATIC",
            L"디스코드에서는 'PadLink 화면' 창을 공유하세요(디스코드용 출력은 내가 안 듣는 장치로, 이 앱 음소거 금지). "
            L"펜: 스퀴즈 = Space, 더블탭 = E/P (ini에서 변경). 클립 스튜디오 태블릿 설정은 TabletPC. "
            L"iPad에서 펜 모드와 송출은 하나만 켜집니다.",
            SS_LEFT, 12, y, 700, 40, -1);

    y += 46;
    Control(L"BUTTON", L"적용", BS_PUSHBUTTON | WS_TABSTOP, 12, y, 110, 28, ID_APPLY);
    Control(L"BUTTON", L"화면 창 보이기", BS_PUSHBUTTON | WS_TABSTOP, 130, y, 120, 28, ID_SHOW_VIDEO);
    Control(L"BUTTON", L"화면 90° 회전", BS_PUSHBUTTON | WS_TABSTOP, 258, y, 120, 28, ID_ROTATE);

    y += 38;
    Control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 12, y, 700, 300, ID_LOG,
            WS_EX_CLIENTEDGE);
}

void InitAudioUi() {
    std::wstring error;
    if (!g_audio.Load(error)) {
        Log(error);
        for (int id : {ID_ASIO, ID_DISC}) EnableWindow(Item(id), FALSE);
        return;
    }
    auto asioNames = g_audio.Drivers(true);
    auto wasapiNames = g_audio.Drivers(false);
    Log(Format(L"ASIO 드라이버 %zu개, 출력 장치 %zu개", asioNames.size(), wasapiNames.size()));
    if (g_settings.discordDevice.empty()) {
        g_settings.discordDevice = GuessDiscordDevice(wasapiNames);
        if (!g_settings.discordDevice.empty()) Log(L"디스코드용 출력 기본값: " + g_settings.discordDevice);
    }
    FillCombo(ID_ASIO, asioNames, g_settings.asioDriver, L"(첫 번째 ASIO 드라이버)");
    FillCombo(ID_DISC, wasapiNames, g_settings.discordDevice, L"(사용 안 함)");
    g_audio.Start();
    ApplyAudio();
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_CONNECT:
            ToggleConnect();
            return 0;
        case ID_APPLY: {
            int oldDisplay = g_settings.displayBitrateMbps;
            ReadUi();
            SaveSettings(g_settings);
            ApplyAudio();
            if (g_settings.displayBitrateMbps != oldDisplay && g_screen.Running()) {
                Log(Format(L"액정타블렛 화질 %dMbps로 다시 시작", g_settings.displayBitrateMbps));
                UpdateScreenSender(true);
            }
            if (g_receiver.connected) SendBroadcastConfig();
            return 0;
        }
        case ID_SHOW_VIDEO:
            ShowWindow(g_video, SW_SHOWNORMAL);
            SetForegroundWindow(g_video);
            return 0;
        case ID_ROTATE:
            g_settings.rotation = g_videoPipe.RotateUser();
            SaveSettings(g_settings);
            return 0;
        case ID_PEN_MON:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                ApplyPen();
                SaveSettings(g_settings);
                UpdateScreenSender(g_screen.Running());
                VddTick();
            }
            return 0;
        case ID_DISPLAY:
            if (g_settings.penMonitor == kVddChoice) return 0;
            g_settings.display = SendMessageW(Item(ID_DISPLAY), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
            SaveSettings(g_settings);
            UpdateScreenSender(false);
            VddTick();
            return 0;
        }
        break;
    case WM_DISPLAYCHANGE:
        // 해상도·배치가 바뀌었거나 VDD가 켜지고 꺼졌다
        FillMonitors();
        ApplyPen();
        UpdateScreenSender(g_screen.Running());
        return 0;
    case WM_APP_PEN_LINK:
        UpdateScreenSender(false);
        VddTick();
        return 0;
    case WM_APP_VDD_DONE: {
        std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lp));
        g_vddBusy = false;
        bool on = wp == 1;
        if (error) {
            Log(L"VDD: " + *error);
        } else {
            Log(on ? L"VDD: 가상 모니터 켬" : L"VDD: 가상 모니터 끔");
            SetVddOwned(on);
        }
        FillMonitors();
        ApplyPen();
        UpdateScreenSender(false);
        return 0;
    }
    case WM_TIMER:
        if (wp == kStatsTimer) {
            VddTick();
            UpdateStatus();
        }
        return 0;
    case WM_APP_LOG: {
        std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
        AppendLog(*line);
        return 0;
    }
    case WM_APP_STATUS: {
        std::unique_ptr<std::wstring> status(reinterpret_cast<std::wstring*>(lp));
        if (g_receiver.Running()) (wp == 1 ? g_penStatus : g_linkStatus) = *status;
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        // 읽기 전용 로그 칸도 흰 배경으로
        if (reinterpret_cast<HWND>(lp) == Item(ID_LOG)) {
            SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        break;
    case WM_DESTROY:
        KillTimer(hwnd, kStatsTimer);
        g_screen.Stop();
        if ((g_settings.vddOwned || g_settings.penMonitor == kVddChoice) && VddQuery().active) {
            std::wstring error;
            if (VddSetActive(false, error)) Log(L"VDD: 종료하면서 가상 모니터를 끔");
            else Log(L"VDD: " + error);
        }
        SetVddOwned(false);
        LogSetWindow(nullptr, 0);
        ReadUi();
        SaveSettings(g_settings);
        g_screen.Stop();
        g_receiver.Stop();
        g_penLink.Stop();
        g_pen.Reset();
        g_audio.Stop();
        g_videoPipe.Stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK VideoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_videoOk) g_videoPipe.RequestRedraw();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        if (g_videoOk) g_videoPipe.RequestRedraw();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        if (wp == 'R') {
            g_settings.rotation = g_videoPipe.RotateUser();
            SaveSettings(g_settings);
        } else if (wp == 'C') {
            g_videoPipe.ToggleRange();
        }
        return 0;
    case WM_CLOSE:
        // 창을 닫으면 디스코드 공유가 끊기니 숨기기만 한다
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g_instance = instance;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    LogInit();
    g_settings = LoadSettings();
    g_scale = GetDpiForSystem() / 96.0f;
    g_font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"PadLinkMain";
    RegisterClassExW(&wc);

    WNDCLASSEXW vc{sizeof(vc)};
    vc.lpfnWndProc = VideoProc;
    vc.hInstance = instance;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    vc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    vc.lpszClassName = L"PadLinkVideo";
    RegisterClassExW(&vc);

    DWORD mainStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, S(724), S(678)};
    AdjustWindowRect(&rc, mainStyle, FALSE);
    g_main = CreateWindowExW(0, L"PadLinkMain", L"PadLink 수신", mainStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, instance, nullptr);
    CreateControls();
    LogSetWindow(g_main, WM_APP_LOG);
    Log(L"PadLink 수신 시작");

    // iPad Air 11" 화면(2360x1640)의 절반 크기로 시작
    RECT vr{0, 0, 1180, 820};
    AdjustWindowRect(&vr, WS_OVERLAPPEDWINDOW, FALSE);
    g_video = CreateWindowExW(0, L"PadLinkVideo", L"PadLink 화면", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              vr.right - vr.left, vr.bottom - vr.top, nullptr, nullptr, instance, nullptr);
    g_videoOk = g_videoPipe.Start(g_video, g_settings.rotation);

    std::wstring penError;
    g_penOk = g_pen.Init(penError);
    if (!g_penOk) Log(penError);
    VddRepairConfig();
    FillMonitors();
    ApplyPen();
    // 기본은 꺼 둔다: 자동 모드인데 켜져 있으면 바로 끈다
    if ((g_settings.penMonitor == kVddChoice || g_settings.vddOwned) && g_vdd.active) {
        g_vddUnneededSince = 1;
        VddTick();
    }

    InitAudioUi();
    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    SetTimer(g_main, kStatsTimer, 1000, nullptr);
    UpdateStatus();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_main, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(g_font);
    CoUninitialize();
    WSACleanup();
    return 0;
}
