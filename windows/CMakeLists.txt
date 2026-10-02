cmake_minimum_required(VERSION 3.21)
project(PadLinkRecv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 정적 CRT: 받는 사람이 VC++ 재배포 패키지를 깔 필요 없게
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(PadLinkRecv WIN32
  src/main.cpp
  src/util.cpp
  src/log.cpp
  src/settings.cpp
  src/plist.cpp
  src/usbmux.cpp
  src/net.cpp
  src/video.cpp
  src/audio.cpp
  src/pen.cpp
  src/app.manifest
)
target_compile_definitions(PadLinkRecv PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
  # 합성 펜(InjectSyntheticPointerInput)은 Windows 10 1809(RS5) API
  NTDDI_VERSION=0x0A000006)
target_compile_options(PadLinkRecv PRIVATE /utf-8 /W4 /permissive- /EHsc)
target_link_libraries(PadLinkRecv PRIVATE
  ws2_32 d3d11 dxgi mfplat mfuuid wmcodecdspuuid strmiids comctl32 ole32 winmm)
