# PadLink

iPad Air(M3) + Apple Pencil Pro를 Windows PC의 액정타블렛·송출 장치로 쓰기 위한 개인 프로젝트.
Mac과 유료 Apple 개발자 계정 없이, GitHub Actions로 빌드하고 SideStore로 설치한다.

## 구성

- `ios/` — iPad 앱 (Swift, XcodeGen)
  - `Broadcast/` — 화면 방송 확장: iPad 전체 화면 + 앱 소리 → H.264/PCM → TCP
  - `App/` — 방송 시작 버튼, 접속 정보
- `docs/protocol.md` — PC와 주고받는 형식
- `windows/` — 수신 앱 (C++/Win32): USB(usbmuxd)·Wi-Fi 연결, 하드웨어 H.264 디코딩 + D3D11 화면 창,
  FMOD로 ASIO(오인페, 내 모니터링) / WASAPI(디스코드 캡처용) 동시 출력

## 설치 (iPad)

1. PC에서 한 번만 SideStore를 설치한다 (SideStore 공식 가이드).
2. iPad Safari에서 최신 빌드를 받는다:
   `https://github.com/sungwoo0829/padlink/releases/download/nightly/PadLink.ipa`
3. 받은 파일을 SideStore로 열어 설치한다. (무료 계정: 7일마다 SideStore가 갱신)

## 설치 (Windows)

`https://github.com/sungwoo0829/padlink/releases/download/win-nightly/PadLinkRecv-win64.zip`
압축을 풀고 `README-windows.txt`대로 `fmod.dll`을 옆에 두면 된다.

## 확인 (PC, 같은 Wi-Fi, 수신 앱 없이)

iPad에서 PadLink를 열고 방송을 시작한 뒤:

```
ffplay -fflags nobuffer -flags low_delay -framedrop -f h264 tcp://<iPad IP>:47801
ffplay -nodisp -f wav tcp://<iPad IP>:47802
```

## 로드맵

1. 송출: iPad → PC 영상·소리 (실기기 확인)
2. Windows 수신 앱: USB(usbmux) 연결, 영상 창, FMOD ASIO + WASAPI 두 갈래 출력 (진행 중)
3. 펜 입력: 필압·기울기·배럴 롤·호버 → Windows 합성 펜, 스퀴즈·더블탭 → 단축키 (실기기 확인)
4. 액정타블렛: 모니터(VDD 가상 모니터) 캡처 → BT.709 NV12 → NVENC → 펜 채널로 iPad 표시 (진행 중)
