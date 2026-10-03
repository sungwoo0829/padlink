#include "vdd.h"

#include <cfgmgr32.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <regex>
#include <sstream>

#include "log.h"

namespace {
constexpr wchar_t kPipe[] = L"\\\\.\\pipe\\MTTVirtualDisplayPipe";

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

// \\?\ROOT#DISPLAY#0000#{guid} → ROOT\DISPLAY\0000
std::wstring InstanceIdFromPath(std::wstring path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) path = path.substr(4);
    size_t guid = path.find(L"#{");
    if (guid != std::wstring::npos) path = path.substr(0, guid);
    std::replace(path.begin(), path.end(), L'#', L'\\');
    return path;
}

// 어댑터의 PnP 하드웨어 ID가 Root\MttVDD인지 (설치기에 따라 인스턴스 경로는 ROOT\DISPLAY\... 일 수도 있다)
bool IsVddAdapter(const LUID& adapterId) {
    DISPLAYCONFIG_ADAPTER_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = adapterId;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) return false;
    std::wstring path = name.adapterDevicePath;
    if (Lower(path).find(L"mttvdd") != std::wstring::npos) return true;

    std::wstring id = InstanceIdFromPath(path);
    DEVINST inst = 0;
    if (CM_Locate_DevNodeW(&inst, id.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return false;
    wchar_t ids[2048] = {};
    ULONG size = sizeof(ids) - sizeof(wchar_t) * 2;
    ULONG type = 0;
    if (CM_Get_DevNode_Registry_PropertyW(inst, CM_DRP_HARDWAREID, &type, ids, &size, 0) != CR_SUCCESS) return false;
    for (const wchar_t* p = ids; *p; p += wcslen(p) + 1)
        if (Lower(p).find(L"mttvdd") != std::wstring::npos) return true;
    return false;
}

bool IsVddTarget(const DISPLAYCONFIG_PATH_TARGET_INFO& target) {
    if (IsVddAdapter(target.adapterId)) return true;
    DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = target.adapterId;
    name.header.id = target.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) return false;
    std::wstring friendly = Lower(name.monitorFriendlyDeviceName);
    return friendly.find(L"vdd") != std::wstring::npos || friendly.find(L"virtual display") != std::wstring::npos;
}

bool SameLuid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

struct Config {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

bool QueryAll(Config& c) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &np, &nm) != ERROR_SUCCESS) return false;
        c.paths.resize(np);
        c.modes.resize(nm);
        LONG r = QueryDisplayConfig(QDC_ALL_PATHS, &np, c.paths.data(), &nm, c.modes.data(), nullptr);
        if (r == ERROR_INSUFFICIENT_BUFFER) continue;  // 그 사이 구성이 바뀜
        if (r != ERROR_SUCCESS) return false;
        c.paths.resize(np);
        c.modes.resize(nm);
        return true;
    }
    return false;
}

std::wstring SourceGdiName(const DISPLAYCONFIG_PATH_SOURCE_INFO& source) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = source.adapterId;
    name.header.id = source.id;
    return DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS ? name.viewGdiDeviceName : L"";
}

bool IsActive(const DISPLAYCONFIG_PATH_INFO& p) { return (p.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0; }

std::wstring VddConfigPath() {
    std::wstring dir = L"C:\\VirtualDisplayDriver";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver", 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        wchar_t path[MAX_PATH] = {};
        DWORD size = sizeof(path) - sizeof(wchar_t);
        if (RegQueryValueExW(key, L"VDDPATH", nullptr, nullptr, reinterpret_cast<LPBYTE>(path), &size) ==
                ERROR_SUCCESS &&
            path[0])
            dir = path;
        RegCloseKey(key);
    }
    return dir + L"\\vdd_settings.xml";
}

bool SendPipeCommand(const std::wstring& command, std::wstring& error) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE pipe = CreateFileW(kPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
            DWORD written = 0;
            BOOL ok = WriteFile(pipe, command.data(), DWORD(command.size() * sizeof(wchar_t)), &written, nullptr);
            DWORD e = GetLastError();
            CloseHandle(pipe);
            if (!ok && e != ERROR_NO_DATA && e != ERROR_PIPE_NOT_CONNECTED) {
                error = Format(L"VDD 명령 쓰기 실패 (오류 %lu)", e);
                return false;
            }
            return true;
        }
        DWORD e = GetLastError();
        if (e != ERROR_PIPE_BUSY) {
            error = Format(L"VDD 제어 파이프를 열 수 없음 (오류 %lu)", e);
            return false;
        }
        WaitNamedPipeW(kPipe, 500);
    }
    error = L"VDD 제어 파이프가 응답하지 않음";
    return false;
}
}  // namespace

VddState VddQuery() {
    VddState s;
    Config c;
    if (!QueryAll(c)) return s;
    for (const auto& p : c.paths) {
        if (!p.targetInfo.targetAvailable || !IsVddTarget(p.targetInfo)) continue;
        s.installed = true;
        if (IsActive(p)) {
            s.active = true;
            s.device = SourceGdiName(p.sourceInfo);
            break;
        }
    }
    return s;
}

bool VddSetActive(bool active, std::wstring& error) {
    Config c;
    if (!QueryAll(c)) {
        error = L"디스플레이 구성을 읽을 수 없음";
        return false;
    }
    // 지금 켜져 있는 경로들 (VDD는 끄려면 빼고, 켜려면 하나 더한다)
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    const DISPLAYCONFIG_PATH_INFO* candidate = nullptr;
    bool vddActive = false;
    for (const auto& p : c.paths) {
        bool vdd = p.targetInfo.targetAvailable && IsVddTarget(p.targetInfo);
        if (IsActive(p)) {
            if (vdd) {
                vddActive = true;
                if (!active) continue;
            }
            paths.push_back(p);
        }
    }
    if (vddActive == active) return true;

    if (active) {
        // 다른 켜진 경로가 쓰지 않는 소스(같은 어댑터 기준)에 VDD 타깃을 붙인다
        for (const auto& p : c.paths) {
            if (IsActive(p) || !p.targetInfo.targetAvailable || !IsVddTarget(p.targetInfo)) continue;
            bool sourceUsed = std::any_of(paths.begin(), paths.end(), [&](const DISPLAYCONFIG_PATH_INFO& a) {
                return SameLuid(a.sourceInfo.adapterId, p.sourceInfo.adapterId) && a.sourceInfo.id == p.sourceInfo.id;
            });
            if (!sourceUsed) {
                candidate = &p;
                break;
            }
        }
        if (!candidate) {
            error = L"VDD 모니터를 붙일 경로를 찾지 못함";
            return false;
        }
        DISPLAYCONFIG_PATH_INFO p = *candidate;
        p.flags = DISPLAYCONFIG_PATH_ACTIVE;
        p.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        paths.push_back(p);
    }

    // 해상도·위치는 Windows가 그 모니터에 대해 기억하던 값으로 채운다
    UINT32 flags = SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE;
    LONG r = SetDisplayConfig(UINT32(paths.size()), paths.data(), UINT32(c.modes.size()), c.modes.data(), flags);
    if (r != ERROR_SUCCESS) {
        error = Format(L"디스플레이 구성 변경 실패 (오류 %ld)", r);
        return false;
    }
    return true;
}

void VddRepairConfig() {
    std::wstring path = VddConfigPath();
    std::ifstream file(path, std::ios::binary);
    if (!file) return;
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string xml = buffer.str();
    std::smatch m;
    static const std::regex countRe(R"(<monitors>\s*<count>\s*(\d+)\s*</count>)");
    if (!std::regex_search(xml, m, countRe) || std::stoi(m[1].str()) != 0) return;
    std::wstring error;
    if (SendPipeCommand(L"SETDISPLAYCOUNT 1", error)) {
        Log(L"VDD: 설정 파일의 모니터 수가 0이라 1로 되돌림 (재부팅 뒤에도 가상 모니터가 생기도록)");
    } else {
        Log(L"VDD: 설정 파일의 모니터 수가 0인데 되돌리지 못함 — VDD 제어 앱에서 모니터 수를 1로 바꾸세요 (" + error +
            L")");
    }
}
