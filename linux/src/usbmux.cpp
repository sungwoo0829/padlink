#include "usbmux.h"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "plist.h"

namespace {
#pragma pack(push, 1)
struct MuxHeader {
    uint32_t length;   // 헤더 포함 전체 길이
    uint32_t version;  // 1 = plist
    uint32_t message;  // 8 = plist
    uint32_t tag;
};
#pragma pack(pop)

void SetRecvTimeout(int fd, int seconds) {
    timeval tv{seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// USBMUXD_SOCKET_ADDRESS가 있으면 따른다 ("UNIX:/경로" 또는 "호스트:포트")
int ConnectMuxd(std::string& error) {
    std::string address = "UNIX:/var/run/usbmuxd";
    if (const char* env = std::getenv("USBMUXD_SOCKET_ADDRESS"); env && *env) address = env;
    int fd = -1;
    if (address.rfind("UNIX:", 0) == 0) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, address.c_str() + 5, sizeof(addr.sun_path) - 1);
        if (fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(fd);
            fd = -1;
        }
    } else {
        size_t colon = address.rfind(':');
        std::string host = address.substr(0, colon), port = colon == std::string::npos ? "27015" : address.substr(colon + 1);
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) == 0 && result) {
            fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
            if (fd >= 0 && connect(fd, result->ai_addr, result->ai_addrlen) != 0) {
                close(fd);
                fd = -1;
            }
            freeaddrinfo(result);
        }
    }
    if (fd < 0) {
        error = "usbmuxd에 연결할 수 없음 — 'sudo apt install usbmuxd' 후 iPad를 USB로 꽂으세요";
        return -1;
    }
    SetRecvTimeout(fd, 5);
    return fd;
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

bool SendPlist(int fd, const std::string& body, uint32_t tag) {
    MuxHeader h{uint32_t(sizeof(MuxHeader) + body.size()), 1, 8, tag};
    return SendAll(fd, &h, sizeof(h)) && SendAll(fd, body.data(), body.size());
}

bool RecvPlist(int fd, PlistValue& out, std::string& error) {
    MuxHeader h{};
    if (!RecvAll(fd, &h, sizeof(h))) {
        error = "usbmuxd 응답 없음";
        return false;
    }
    if (h.length < sizeof(h) || h.length > (4u << 20)) {
        error = "usbmuxd 응답 길이 이상";
        return false;
    }
    std::string body(h.length - sizeof(h), '\0');
    if (!body.empty() && !RecvAll(fd, body.data(), body.size())) {
        error = "usbmuxd 응답이 잘림";
        return false;
    }
    if (!ParseXmlPlist(body, out)) {
        error = "usbmuxd 응답을 해석할 수 없음";
        return false;
    }
    return true;
}
}  // namespace

bool SendAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n;
        len -= size_t(n);
    }
    return true;
}

bool RecvAll(int fd, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= size_t(n);
    }
    return true;
}

bool UsbmuxList(std::vector<UsbmuxDevice>& out, std::string& error) {
    out.clear();
    int fd = ConnectMuxd(error);
    if (fd < 0) return false;
    PlistValue reply;
    bool ok = SendPlist(fd, Request("ListDevices", ""), 1) && RecvPlist(fd, reply, error);
    close(fd);
    if (!ok) return false;
    const PlistValue* list = reply.Get("DeviceList");
    if (!list || list->type != PlistValue::Type::Array) {
        error = "usbmuxd가 기기 목록을 주지 않음";
        return false;
    }
    for (const PlistValue& entry : list->array) {
        UsbmuxDevice d;
        d.id = int(entry.Integer("DeviceID", 0));
        if (const PlistValue* props = entry.Get("Properties")) {
            if (d.id == 0) d.id = int(props->Integer("DeviceID", 0));
            d.serial = props->String("SerialNumber");
            d.connectionType = props->String("ConnectionType");
        }
        if (d.id != 0) out.push_back(std::move(d));
    }
    return true;
}

int UsbmuxConnect(int deviceId, uint16_t port, int& result, std::string& error) {
    result = -1;
    int fd = ConnectMuxd(error);
    if (fd < 0) return -1;
    // usbmuxd는 포트를 네트워크 바이트 순서로 뒤집은 정수로 받는다
    int swapped = ((port & 0xFF) << 8) | (port >> 8);
    std::string extra = "<key>DeviceID</key><integer>" + std::to_string(deviceId) +
                        "</integer>\n<key>PortNumber</key><integer>" + std::to_string(swapped) + "</integer>\n";
    PlistValue reply;
    if (!SendPlist(fd, Request("Connect", extra), 2) || !RecvPlist(fd, reply, error)) {
        close(fd);
        return -1;
    }
    result = int(reply.Integer("Number", -1));
    if (result != 0) {
        error = "usbmuxd 연결 거부 (코드 " + std::to_string(result) + ")";
        close(fd);
        return -1;
    }
    SetRecvTimeout(fd, 0);
    return fd;
}
