#include "vdd.h"

#include <cfgmgr32.h>

#include <algorithm>
#include <cwctype>
#include <thread>

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

bool LooksLikeVddMonitor(const DISPLAYCONFIG_PATH_TARGET_INFO& target) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = target.adapterId;
    name.header.id = target.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) return false;
    std::wstring friendly = Lower(name.monitorFriendlyDeviceName);
    return friendly.find(L"vdd") != std::wstring::npos || friendly.find(L"virtual display") != std::wstring::npos;
}
}  // namespace

bool VddInstalled() {
    if (WaitNamedPipeW(kPipe, 1)) return true;
    DWORD e = GetLastError();
    return e == ERROR_SEM_TIMEOUT || e == ERROR_PIPE_BUSY;
}

bool VddSetDisplayCount(int count, std::wstring& error) {
    std::wstring command = L"SETDISPLAYCOUNT " + std::to_wstring(std::clamp(count, 0, 16));
    for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE pipe = CreateFileW(kPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
            DWORD written = 0;
            // 드라이버는 명령을 읽자마자 연결을 끊고 다시 시작한다. 응답은 기다리지 않는다.
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
        if (e == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(kPipe, 500);
            continue;
        }
        if (e == ERROR_FILE_NOT_FOUND) {
            // 드라이버가 막 다시 시작하는 중일 수 있다
            Sleep(250);
            continue;
        }
        error = Format(L"VDD 제어 파이프를 열 수 없음 (오류 %lu)", e);
        return false;
    }
    error = L"VDD 제어 파이프가 응답하지 않음 — Virtual Display Driver가 설치·동작 중인지 확인";
    return false;
}

void VddSetDisplayCountAsync(int count) {
    std::thread([count] {
        std::wstring error;
        if (VddSetDisplayCount(count, error)) {
            Log(Format(L"VDD: 가상 모니터 %d개로 설정", count));
        } else {
            Log(L"VDD: " + error);
        }
    }).detach();
}

std::vector<std::wstring> VddMonitorDevices() {
    std::vector<std::wstring> out;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return out;
    paths.resize(pathCount);
    for (const auto& p : paths) {
        bool vdd = IsVddAdapter(p.targetInfo.adapterId) || IsVddAdapter(p.sourceInfo.adapterId) ||
                   LooksLikeVddMonitor(p.targetInfo);
        if (!vdd) continue;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = p.sourceInfo.adapterId;
        source.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) out.push_back(source.viewGdiDeviceName);
    }
    return out;
}
