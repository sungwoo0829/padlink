#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

// 'padlink-recv --save-replay'가 실행 중인 앱에 하이라이트 저장을 부탁하는 통로 (유닉스 소켓).
// GNOME 사용자 지정 단축키에 이 명령을 걸면 다른 앱을 쓰는 중에도 저장된다.
std::string ControlSocketPath();
int SendControlCommand(const std::string& command);  // 프로세스 종료 코드

class ControlServer {
public:
    ~ControlServer() { Stop(); }
    bool Start(std::function<void()> onSave);
    void Stop();

private:
    void Loop();
    int fd_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::function<void()> onSave_;
};

// GNOME 설정에 Ctrl+Alt+H → 'padlink-recv --save-replay' 사용자 지정 단축키를 넣는다
bool RegisterGnomeShortcut(std::string& message);
