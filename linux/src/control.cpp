#include "control.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"

std::string ControlSocketPath() {
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        return std::string(runtime) + "/padlink-recv.sock";
    return "/tmp/padlink-recv-" + std::to_string(getuid()) + ".sock";
}

namespace {
bool FillAddress(sockaddr_un& addr) {
    std::string path = ControlSocketPath();
    if (path.size() >= sizeof(addr.sun_path)) return false;
    addr = sockaddr_un{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

std::string RunCapture(const std::string& command) {
    std::string out;
    if (FILE* p = popen(command.c_str(), "r")) {
        char buf[512];
        while (std::fgets(buf, sizeof(buf), p)) out += buf;
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}
}  // namespace

int SendControlCommand(const std::string& command) {
    sockaddr_un addr{};
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || !FillAddress(addr) || connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "PadLink 수신이 실행 중이 아닙니다.\n");
        if (fd >= 0) close(fd);
        return 1;
    }
    std::string line = command + "\n";
    if (write(fd, line.data(), line.size()) < 0) {
        close(fd);
        return 1;
    }
    char reply[256] = {};
    ssize_t n = read(fd, reply, sizeof(reply) - 1);
    close(fd);
    if (n > 0) std::printf("%s", reply);
    return n > 0 && std::strncmp(reply, "ok", 2) == 0 ? 0 : 1;
}

bool ControlServer::Start(std::function<void()> onSave) {
    onSave_ = std::move(onSave);
    sockaddr_un addr{};
    if (!FillAddress(addr)) return false;
    unlink(addr.sun_path);  // 지난번에 비정상 종료하며 남은 것
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0 || bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd_, 4) != 0) {
        Log("하이라이트 단축키 통로를 열 수 없음: " + ControlSocketPath());
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        return false;
    }
    stop_ = false;
    thread_ = std::thread([this] { Loop(); });
    return true;
}

void ControlServer::Stop() {
    stop_ = true;
    if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
        unlink(ControlSocketPath().c_str());
    }
}

void ControlServer::Loop() {
    while (!stop_) {
        int client = accept(fd_, nullptr, nullptr);
        if (client < 0) {
            if (stop_) break;
            continue;
        }
        char buf[64] = {};
        ssize_t n = read(client, buf, sizeof(buf) - 1);
        std::string reply = "error: 알 수 없는 명령\n";
        if (n > 0 && std::strncmp(buf, "save", 4) == 0) {
            if (onSave_) onSave_();
            reply = "ok: 하이라이트 저장을 시작함\n";
        }
        ssize_t sent = write(client, reply.data(), reply.size());
        (void)sent;
        close(client);
    }
}

bool RegisterGnomeShortcut(std::string& message) {
    char exe[512] = {};
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    std::string command = len > 0 ? std::string(exe, size_t(len)) : "padlink-recv";
    const std::string base = "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/padlink/";
    const std::string listKey = "org.gnome.settings-daemon.plugins.media-keys custom-keybindings";
    std::string list = RunCapture("gsettings get " + listKey + " 2>/dev/null");
    if (list.empty()) {
        message = "gsettings를 찾을 수 없음 (GNOME이 아니면 데스크톱 설정에서 직접 단축키를 만드세요)";
        return false;
    }
    if (list.find(base) == std::string::npos) {
        std::string updated;
        if (list.find('\'') == std::string::npos) {
            updated = "['" + base + "']";  // "@as []" 처럼 비어 있음
        } else {
            size_t close = list.rfind(']');
            updated = list.substr(0, close) + ", '" + base + "']";
        }
        if (std::system(("gsettings set " + listKey + " \"" + updated + "\"").c_str()) != 0) {
            message = "단축키 목록을 바꾸지 못함";
            return false;
        }
    }
    const std::string schema = "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:" + base;
    bool ok = std::system(("gsettings set " + schema + " name \"'PadLink 하이라이트 저장'\"").c_str()) == 0 &&
              std::system(("gsettings set " + schema + " command \"'" + command + " --save-replay'\"").c_str()) == 0 &&
              std::system(("gsettings set " + schema + " binding \"'<Control><Alt>h'\"").c_str()) == 0;
    message = ok ? "GNOME 단축키 등록: Ctrl+Alt+H → 하이라이트 저장" : "단축키 설정을 쓰지 못함";
    return ok;
}
