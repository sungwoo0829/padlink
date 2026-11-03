#pragma once
#include <cstdint>
#include <string>
#include <vector>

// 설정 폴더의 padlink-recv.log와 화면의 로그 칸에 함께 쓴다 (어느 스레드에서든 호출 가능)
void LogInit(const std::string& path);
void Log(const std::string& line);
// 화면 표시용: 최근 줄들과, 바뀔 때마다 늘어나는 번호
std::vector<std::string> LogLines();
uint64_t LogVersion();

std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
