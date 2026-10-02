#include "pen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>

#include "log.h"

namespace {
// iPad → PC 펜 샘플 (docs/protocol.md)
#pragma pack(push, 1)
struct WireSample {
    uint8_t phase;
    uint8_t flags;
    uint16_t reserved;
    float x, y, pressure, altitude, azimuth, roll, z;
};
#pragma pack(pop)
static_assert(sizeof(WireSample) == 32);

enum Phase : uint8_t { kHover = 0, kDown = 1, kMove = 2, kUp = 3, kHoverExit = 4, kCancel = 5 };
enum Button : uint8_t { kSqueeze = 1, kDoubleTap = 2 };
enum ButtonPhase : uint8_t { kBegan = 0, kEnded = 1, kTap = 2 };

constexpr double kPi = 3.14159265358979323846;

// "CTRL+SHIFT+Z", "SPACE", "E" → 가상 키 목록 (수정 키가 앞)
std::vector<WORD> ParseCombo(const std::wstring& text) {
    std::vector<WORD> keys;
    size_t start = 0;
    while (start <= text.size()) {
        size_t plus = text.find(L'+', start);
        std::wstring token = text.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start);
        start = plus == std::wstring::npos ? text.size() + 1 : plus + 1;
        token.erase(std::remove_if(token.begin(), token.end(), [](wchar_t c) { return std::iswspace(c) != 0; }),
                    token.end());
        if (token.empty()) continue;
        std::wstring up = token;
        for (auto& c : up) c = (wchar_t)std::towupper(c);
        WORD vk = 0;
        if (up == L"CTRL" || up == L"CONTROL") vk = VK_CONTROL;
        else if (up == L"SHIFT") vk = VK_SHIFT;
        else if (up == L"ALT") vk = VK_MENU;
        else if (up == L"WIN") vk = VK_LWIN;
        else if (up == L"SPACE") vk = VK_SPACE;
        else if (up == L"TAB") vk = VK_TAB;
        else if (up == L"ESC" || up == L"ESCAPE") vk = VK_ESCAPE;
        else if (up == L"ENTER") vk = VK_RETURN;
        else if (up == L"BACKSPACE") vk = VK_BACK;
        else if (up == L"DEL" || up == L"DELETE") vk = VK_DELETE;
        else if (up == L"LEFT") vk = VK_LEFT;
        else if (up == L"RIGHT") vk = VK_RIGHT;
        else if (up == L"UP") vk = VK_UP;
        else if (up == L"DOWN") vk = VK_DOWN;
        else if (up.size() >= 2 && up[0] == L'F' && std::iswdigit(up[1])) {
            int n = _wtoi(up.c_str() + 1);
            if (n >= 1 && n <= 24) vk = WORD(VK_F1 + n - 1);
        } else if (up.size() == 1) {
            wchar_t c = up[0];
            if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) vk = WORD(c);
            else {
                SHORT scan = VkKeyScanW(token[0]);
                if (scan != -1) vk = WORD(scan & 0xFF);
            }
        }
        if (vk) keys.push_back(vk);
        else Log(L"알 수 없는 키: " + token);
    }
    return keys;
}

std::vector<std::wstring> SplitList(const std::wstring& text) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t comma = text.find(L',', start);
        std::wstring item = text.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        if (!item.empty()) out.push_back(item);
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    return out;
}

void SendKeys(const std::vector<WORD>& keys, bool down) {
    std::vector<INPUT> inputs;
    auto add = [&](WORD vk, bool press) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = press ? 0 : KEYEVENTF_KEYUP;
        if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_DELETE)
            in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        inputs.push_back(in);
    };
    if (down) {
        for (WORD vk : keys) add(vk, true);
    } else {
        for (auto it = keys.rbegin(); it != keys.rend(); ++it) add(*it, false);
    }
    if (!inputs.empty()) SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
}

BOOL CALLBACK AddMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, &mi)) {
        reinterpret_cast<std::vector<MonitorInfo>*>(param)->push_back(
            MonitorInfo{mi.szDevice, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
    }
    return TRUE;
}
}  // namespace

std::vector<MonitorInfo> ListMonitors() {
    std::vector<MonitorInfo> out;
    EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&out));
    return out;
}

PenInjector::~PenInjector() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    Reset();
    if (device_) DestroySyntheticPointerDevice(device_);
}

bool PenInjector::Init(std::wstring& error) {
    device_ = CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
    if (!device_) {
        error = Format(L"합성 펜 장치 생성 실패 (오류 %lu) — Windows 10 1809 이상이 필요", GetLastError());
        return false;
    }
    thread_ = std::thread([this] { Keepalive(); });
    return true;
}

void PenInjector::Configure(const PenMapping& mapping) {
    std::lock_guard lock(mutex_);
    mapping_ = mapping;
}

RECT PenInjector::Target() {
    std::lock_guard lock(mutex_);
    return mapping_.target;
}

void PenInjector::OnSamples(const uint8_t* data, size_t len) {
    if (len < 4) return;
    uint16_t count = 0;
    std::memcpy(&count, data, 2);
    if (len < 4 + size_t(count) * sizeof(WireSample)) return;
    std::lock_guard lock(mutex_);
    lastData_ = GetTickCount64();
    for (uint16_t i = 0; i < count; ++i) {
        WireSample w;
        std::memcpy(&w, data + 4 + size_t(i) * sizeof(WireSample), sizeof(w));
        Sample s;
        s.x = std::clamp(double(w.x), 0.0, 1.0);
        s.y = std::clamp(double(w.y), 0.0, 1.0);
        s.pressure = std::clamp(double(w.pressure), 0.0, 1.0);
        s.altitude = w.altitude;
        s.azimuth = w.azimuth;
        s.roll = std::isfinite(w.roll) ? w.roll : 0.0;
        HandleLocked(w.phase, s);
    }
    samples += count;
}

// 상태에 맞춰 Windows가 받아들이는 플래그 조합으로만 보낸다
void PenInjector::HandleLocked(uint8_t phase, const Sample& s) {
    constexpr POINTER_FLAGS kHoverFlags = POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE;
    constexpr POINTER_FLAGS kDownFlags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN;
    constexpr POINTER_FLAGS kMoveFlags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE;
    switch (phase) {
    case kHover:
        if (state_ == State::Contact) return;  // 호버 끝 신호가 늦게 와도 획을 끊지 않는다
        InjectLocked(kHoverFlags, s);
        state_ = State::Hover;
        break;
    case kDown:
    case kMove:
        InjectLocked(state_ == State::Contact ? kMoveFlags : kDownFlags, s);
        state_ = State::Contact;
        break;
    case kUp:
        if (state_ != State::Contact) return;
        InjectLocked(POINTER_FLAG_INRANGE | POINTER_FLAG_UP, s);
        state_ = State::Hover;
        break;
    case kHoverExit:
        if (state_ != State::Hover) return;
        InjectLocked(POINTER_FLAG_UPDATE, s);
        state_ = State::Out;
        break;
    case kCancel:
        if (state_ == State::Contact) InjectLocked(POINTER_FLAG_UP | POINTER_FLAG_CANCELED, s);
        state_ = State::Out;
        break;
    default:
        break;
    }
}

bool PenInjector::InjectLocked(POINTER_FLAGS flags, const Sample& s) {
    last_ = s;
    if (!device_) return false;
    const RECT& t = mapping_.target;
    LONG w = std::max<LONG>(t.right - t.left, 1), h = std::max<LONG>(t.bottom - t.top, 1);

    POINTER_TYPE_INFO info{};
    info.type = PT_PEN;
    POINTER_PEN_INFO& pen = info.penInfo;
    pen.pointerInfo.pointerType = PT_PEN;
    pen.pointerInfo.pointerId = 0;
    pen.pointerInfo.pointerFlags = flags;
    pen.pointerInfo.ptPixelLocation.x = t.left + LONG(std::lround(s.x * (w - 1)));
    pen.pointerInfo.ptPixelLocation.y = t.top + LONG(std::lround(s.y * (h - 1)));
    pen.penFlags = PEN_FLAG_NONE;
    pen.penMask = PEN_MASK_PRESSURE | PEN_MASK_ROTATION;

    bool contact = (flags & POINTER_FLAG_INCONTACT) != 0;
    double p = s.pressure;
    if (mapping_.pressureGamma != 100 && p > 0) p = std::pow(p, mapping_.pressureGamma / 100.0);
    pen.pressure = contact ? UINT32(std::clamp(std::lround(p * 1024.0), 1L, 1024L)) : 0;

    double rollDeg = std::fmod(s.roll * 180.0 / kPi, 360.0);
    if (rollDeg < 0) rollDeg += 360.0;
    pen.rotation = UINT32(rollDeg) % 360;

    // altitude: 0 = 눕힘, π/2 = 수직. azimuth: 펜 몸통이 기운 방향 (0 = 오른쪽, y는 아래가 +)
    double tanAlt = std::tan(std::max(s.altitude, 0.02));
    double tx = std::atan(std::cos(s.azimuth) / tanAlt) * 180.0 / kPi;
    double ty = std::atan(std::sin(s.azimuth) / tanAlt) * 180.0 / kPi;
    if (mapping_.invertTilt) {
        tx = -tx;
        ty = -ty;
    }
    INT32 tiltX = INT32(std::clamp(std::lround(tx), -90L, 90L));
    INT32 tiltY = INT32(std::clamp(std::lround(ty), -90L, 90L));
    if (mapping_.tilt) {
        pen.penMask |= PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
        pen.tiltX = tiltX;
        pen.tiltY = tiltY;
    }

    live_.any = true;
    live_.pressure = contact ? pen.pressure / 1024.0 : 0;
    live_.tiltX = tiltX;
    live_.tiltY = tiltY;
    live_.rotation = int(pen.rotation);
    live_.altitudeDeg = s.altitude * 180.0 / kPi;
    if (!range_.any) {
        range_ = live_;
        range_.minTiltX = range_.maxTiltX = tiltX;
        range_.minTiltY = range_.maxTiltY = tiltY;
        range_.maxPressure = live_.pressure;
    } else {
        range_.minTiltX = std::min(range_.minTiltX, int(tiltX));
        range_.maxTiltX = std::max(range_.maxTiltX, int(tiltX));
        range_.minTiltY = std::min(range_.minTiltY, int(tiltY));
        range_.maxTiltY = std::max(range_.maxTiltY, int(tiltY));
        range_.maxPressure = std::max(range_.maxPressure, live_.pressure);
    }

    if (!InjectSyntheticPointerInput(device_, &info, 1)) {
        if (!warned_) {
            warned_ = true;
            Log(Format(L"펜 입력 주입 실패 (오류 %lu, 플래그 0x%X)", GetLastError(), (unsigned)flags));
        }
        return false;
    }
    lastInject_ = GetTickCount64();
    return true;
}

// 펜이 가만히 있어도 Windows가 접촉을 끊지 않게 마지막 상태를 다시 보낸다.
// 연결이 멈추면 획이 계속 이어지지 않게 떼어 준다.
void PenInjector::Keepalive() {
    while (!stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard lock(mutex_);
        if (state_ == State::Out) continue;
        ULONGLONG now = GetTickCount64();
        ULONGLONG silent = now - lastData_;
        if (state_ == State::Contact && silent > 1000) {
            InjectLocked(POINTER_FLAG_INRANGE | POINTER_FLAG_UP, last_);
            state_ = State::Hover;
            continue;
        }
        if (state_ == State::Hover && silent > 2000) {
            InjectLocked(POINTER_FLAG_UPDATE, last_);
            state_ = State::Out;
            continue;
        }
        if (now - lastInject_ > 50) {
            InjectLocked(state_ == State::Contact
                             ? POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE
                             : POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE,
                         last_);
        }
    }
}

void PenInjector::OnButton(uint8_t button, uint8_t phase) {
    std::lock_guard lock(mutex_);
    ++buttons;
    if (button == kSqueeze) {
        if (phase == kBegan && !squeezeHeld_) {
            heldKeys_ = mapping_.squeezeKeys;
            SendKeys(ParseCombo(heldKeys_), true);
            squeezeHeld_ = true;
        } else if (phase == kEnded && squeezeHeld_) {
            ReleaseKeysLocked();
        }
    } else if (button == kDoubleTap && phase == kTap) {
        auto list = SplitList(mapping_.doubleTapKeys);
        if (list.empty()) return;
        doubleTapIndex_ %= list.size();
        auto keys = ParseCombo(list[doubleTapIndex_]);
        SendKeys(keys, true);
        SendKeys(keys, false);
        Log(L"더블탭 → " + list[doubleTapIndex_]);
        doubleTapIndex_ = (doubleTapIndex_ + 1) % list.size();
    }
}

void PenInjector::ReleaseKeysLocked() {
    if (!squeezeHeld_) return;
    SendKeys(ParseCombo(heldKeys_), false);
    squeezeHeld_ = false;
}

PenStats PenInjector::Live() {
    std::lock_guard lock(mutex_);
    return live_;
}

PenStats PenInjector::TakeRange() {
    std::lock_guard lock(mutex_);
    PenStats r = range_;
    range_ = PenStats{};
    return r;
}

void PenInjector::Reset() {
    std::lock_guard lock(mutex_);
    if (state_ == State::Contact) InjectLocked(POINTER_FLAG_INRANGE | POINTER_FLAG_UP, last_);
    if (state_ != State::Out) InjectLocked(POINTER_FLAG_UPDATE, last_);
    state_ = State::Out;
    ReleaseKeysLocked();
}
