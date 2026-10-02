#include "usbmux.h"

#include "plist.h"

namespace {
constexpr uint16_t kMuxdPort = 27015;

#pragma pack(push, 1)
struct MuxHeader {
    uint32_t length;   // 헤더 포함 전체 길이
    uint32_t version;  // 1 = plist
    uint32_t message;  // 8 = plist
    uint32_t tag;
};
#pragma pack(pop)

SOCKET ConnectMuxd(std::wstring& error) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        error = L"소켓 생성 실패";
        return INVALID_SOCKET;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kMuxdPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = L"usbmuxd(127.0.0.1:27015)에 연결할 수 없음 — Microsoft Store의 'Apple Devices' 앱(또는 iTunes)을 설치하세요";
        closesocket(s);
        return INVALID_SOCKET;
    }
    DWORD timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return s;
}

std::string Request(const char* messageType, const std::string& extra) {
    std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "<key>ClientVersionString</key><string>PadLink</string>\n"
        "<key>MessageType</key><string>";
    body += messageType;
    body +=
        "</string>\n"
        "<key>ProgName</key><string>PadLink</string>\n"
        "<key>kLibUSBMuxVersion</key><integer>3</integer>\n";
    body += extra;
    body += "</dict>\n</plist>\n";
    return body;
}

bool SendPlist(SOCKET s, const std::string& body, uint32_t tag) {
    MuxHeader h{uint32_t(sizeof(MuxHeader) + body.size()), 1, 8, tag};
    return SendAll(s, &h, sizeof(h)) && SendAll(s, body.data(), body.size());
}

bool RecvPlist(SOCKET s, PlistValue& out, std::wstring& error) {
    MuxHeader h{};
    if (!RecvAll(s, &h, sizeof(h))) {
        error = L"usbmuxd 응답 없음";
        return false;
    }
    if (h.length < sizeof(h) || h.length > (4u << 20)) {
        error = L"usbmuxd 응답 길이 이상";
        return false;
    }
    std::string body(h.length - sizeof(h), '\0');
    if (!body.empty() && !RecvAll(s, body.data(), body.size())) {
        error = L"usbmuxd 응답이 잘림";
        return false;
    }
    if (!ParseXmlPlist(body, out)) {
        error = L"usbmuxd 응답을 해석할 수 없음";
        return false;
    }
    return true;
}
}  // namespace

bool UsbmuxList(std::vector<UsbmuxDevice>& out, std::wstring& error) {
    out.clear();
    SOCKET s = ConnectMuxd(error);
    if (s == INVALID_SOCKET) return false;
    PlistValue reply;
    bool ok = SendPlist(s, Request("ListDevices", ""), 1) && RecvPlist(s, reply, error);
    closesocket(s);
    if (!ok) return false;
    const PlistValue* list = reply.Get("DeviceList");
    if (!list || list->type != PlistValue::Type::Array) {
        error = L"usbmuxd가 기기 목록을 주지 않음";
        return false;
    }
    for (const PlistValue& entry : list->array) {
        UsbmuxDevice d;
        d.id = (int)entry.Integer("DeviceID", 0);
        if (const PlistValue* props = entry.Get("Properties")) {
            if (d.id == 0) d.id = (int)props->Integer("DeviceID", 0);
            d.serial = props->String("SerialNumber");
            d.connectionType = props->String("ConnectionType");
        }
        if (d.id != 0) out.push_back(std::move(d));
    }
    return true;
}

SOCKET UsbmuxConnect(int deviceId, uint16_t port, int& result, std::wstring& error) {
    result = -1;
    SOCKET s = ConnectMuxd(error);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    // usbmuxd는 포트를 네트워크 바이트 순서로 뒤집은 정수로 받는다
    int swapped = ((port & 0xFF) << 8) | (port >> 8);
    std::string extra = "<key>DeviceID</key><integer>" + std::to_string(deviceId) +
                        "</integer>\n<key>PortNumber</key><integer>" + std::to_string(swapped) + "</integer>\n";
    PlistValue reply;
    if (!SendPlist(s, Request("Connect", extra), 2) || !RecvPlist(s, reply, error)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    result = (int)reply.Integer("Number", -1);
    if (result != 0) {
        error = Format(L"usbmuxd 연결 거부 (코드 %d)", result);
        closesocket(s);
        return INVALID_SOCKET;
    }
    DWORD noTimeout = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&noTimeout), sizeof(noTimeout));
    return s;
}
