#include "common.h"

#include <commctrl.h>
#include <objbase.h>

#include "audio.h"
#include "log.h"
#include "net.h"
#include "settings.h"
#include "video.h"

namespace {
constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_STATUS = WM_APP + 2;
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
    ID_APPLY,
    ID_SHOW_VIDEO,
    ID_ROTATE,
    ID_LOG,
};

HINSTANCE g_instance;
HWND g_main;
HWND g_video;
HFONT g_font;
float g_scale = 1.0f;

Settings g_settings;
Receiver g_receiver;
VideoPipeline g_videoPipe;
AudioEngine g_audio;
bool g_videoOk = false;
std::wstring g_linkStatus = L"연결 안 함";
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

void ToggleConnect() {
    if (g_receiver.Running()) {
        if (!g_lastStatus.empty()) Log(L"마지막 상태: " + g_lastStatus);
        g_receiver.Stop();
        SetWindowTextW(Item(ID_CONNECT), L"연결");
        g_linkStatus = L"연결 안 함";
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
    cb.onStatus = [](const std::wstring& status) {
        auto* copy = new std::wstring(status);
        if (!PostMessageW(g_main, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
    };
    g_receiver.Start(g_settings.mode == 1 ? LinkMode::Wifi : LinkMode::Usb, g_settings.ip, cb);
    SetWindowTextW(Item(ID_CONNECT), L"끊기");
    g_linkStatus = L"연결 중…";
    ShowWindow(g_video, SW_SHOWNOACTIVATE);
}

std::wstring OutputLine(const wchar_t* label, const AudioOutputStats& s) {
    if (!s.active) return Format(L"%ls: %ls", label, s.error.empty() ? L"꺼짐" : s.error.c_str());
    return Format(L"%ls: %ls · 버퍼 %.0fms · 보정 %+.2f%% · 끊김 %u · 건너뜀 %u", label, s.device.c_str(), s.fillMs,
                  s.correctionPct, s.underruns, s.skips);
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
    std::wstring line2, line3;
    if (g_audio.Loaded()) {
        AudioStats a = g_audio.Stats();
        if (a.sourceRate > 0) line1 += Format(L" · 소리 %dHz", a.sourceRate);
        line2 = OutputLine(L"ASIO", a.asio);
        line3 = OutputLine(L"디스코드용", a.discord);
    } else {
        line2 = L"소리 꺼짐 — fmod.dll 필요 (로그 참고)";
    }
    SetWindowTextW(Item(ID_STATUS), (line1 + L"\r\n" + line2 + L"\r\n" + line3).c_str());
    // 원격으로 상태를 볼 수 있게 연결 중에는 1분마다 로그에도 남긴다
    g_lastStatus = line1 + L" | " + line2 + L" | " + line3;
    if (g_receiver.connected && ++g_statusTicks % 60 == 0) Log(L"상태: " + g_lastStatus);
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
    Control(L"STATIC", L"", SS_LEFT, 12, y, 700, 58, ID_STATUS);

    y += 66;
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
    Control(L"STATIC",
            L"디스코드에서는 'PadLink 화면' 창을 공유하세요. 디스코드용 출력은 내가 듣지 않는 장치(VB-CABLE, 디지털 출력 등)로 "
            L"고르고, 볼륨 믹서에서 이 앱을 음소거하지 마세요(디스코드도 무음이 됨).",
            SS_LEFT, 12, y, 700, 40, -1);

    y += 46;
    Control(L"BUTTON", L"오디오 적용", BS_PUSHBUTTON | WS_TABSTOP, 12, y, 110, 28, ID_APPLY);
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
        for (int id : {ID_ASIO, ID_DISC, ID_APPLY}) EnableWindow(Item(id), FALSE);
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
        case ID_APPLY:
            ReadUi();
            SaveSettings(g_settings);
            ApplyAudio();
            return 0;
        case ID_SHOW_VIDEO:
            ShowWindow(g_video, SW_SHOWNORMAL);
            SetForegroundWindow(g_video);
            return 0;
        case ID_ROTATE:
            g_settings.rotation = g_videoPipe.RotateUser();
            SaveSettings(g_settings);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == kStatsTimer) UpdateStatus();
        return 0;
    case WM_APP_LOG: {
        std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
        AppendLog(*line);
        return 0;
    }
    case WM_APP_STATUS: {
        std::unique_ptr<std::wstring> status(reinterpret_cast<std::wstring*>(lp));
        if (g_receiver.Running()) g_linkStatus = *status;
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
        LogSetWindow(nullptr, 0);
        ReadUi();
        SaveSettings(g_settings);
        g_receiver.Stop();
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
    RECT rc{0, 0, S(724), S(568)};
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
