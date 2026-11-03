cmake_minimum_required(VERSION 3.24)
project(PadLinkRecvLinux LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# SDL3: 우분투 24.04 저장소에 없어서 소스로 넣는다. 정적으로 묶고, Wayland·X11·PipeWire 같은
# 백엔드 라이브러리는 실행할 때 불러온다(시스템에 이미 있음).
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.18 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.9b GIT_SHALLOW TRUE)
FetchContent_Declare(stb
  URL https://raw.githubusercontent.com/nothings/stb/d14d46f6b44456eb88beac58cba833a4135d4c89/stb_image_write.h
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(SDL3 imgui stb)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
find_package(Threads REQUIRED)

add_executable(padlink-recv
  src/main.cpp
  src/log.cpp
  src/settings.cpp
  src/usbmux.cpp
  src/net.cpp
  src/decoder.cpp
  src/audio.cpp
  src/snapshot.cpp
  ../shared/plist.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(padlink-recv PRIVATE
  ../shared ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${stb_SOURCE_DIR})
target_compile_options(padlink-recv PRIVATE -Wall -Wextra)
target_link_libraries(padlink-recv PRIVATE SDL3::SDL3-static PkgConfig::FFMPEG Threads::Threads)

install(TARGETS padlink-recv RUNTIME DESTINATION bin)
install(FILES packaging/padlink-recv.desktop DESTINATION share/applications)
