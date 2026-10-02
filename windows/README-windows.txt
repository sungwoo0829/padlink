PadLink 수신 (Windows)
======================

준비
1. fmod.dll
   fmod.com 에서 무료 계정으로 "FMOD Engine" Windows SDK를 받아 설치한 뒤,
   api\core\lib\x64\fmod.dll 을 PadLinkRecv.exe 옆에 복사하세요.
   (없으면 영상만 되고 소리는 꺼집니다.)
2. USB로 쓰려면 Microsoft Store의 "Apple Devices" 앱(또는 iTunes)을 설치하세요.
   iPad를 케이블로 연결하고 iPad에서 "이 컴퓨터 신뢰"를 누릅니다.
3. 디스코드용 출력 장치: 내가 듣지 않는 장치를 고르세요.
   VB-CABLE(무료)의 "CABLE Input"이나 "Realtek Digital Output"처럼 아무것도 안 꽂힌 출력이면 됩니다.

사용
1. iPad에서 PadLink → 방송 버튼 → "PadLink 송출"
2. PadLinkRecv.exe → USB 또는 Wi-Fi(IP 입력) → 연결
3. 디스코드에서 "PadLink 화면" 창을 공유 (소리 포함)

펜 (판타블렛 모드)
1. iPad PadLink 앱 → "펜 모드 시작" (연결은 위와 같은 USB/Wi-Fi로 자동)
2. 수신 앱의 "펜 → 모니터"에서 그릴 모니터를 고르면 iPad 활성 영역이 그 비율로 바뀝니다.
3. 클립 스튜디오: [파일 > 환경 설정 > 태블릿]에서 "TabletPC"를 고르세요(Windows Ink).
4. Windows 설정 > 블루투스 및 장치 > 펜 및 Windows Ink 에서
   "펜을 길게 눌러 마우스 오른쪽 단추 클릭" 같은 펜 제스처를 끄면 그릴 때 끊김이 없습니다.
   (iPad에서는 설정 > Apple Pencil > 손글씨 입력(Scribble)도 끄는 것을 권장)

기본 동작 (PadLinkRecv.ini [pen]에서 변경)
  squeeze=SPACE        스퀴즈를 쥐고 있는 동안 Space → 클립 스튜디오 손 도구
  double_tap=E,P       더블탭마다 E(지우개) / P(펜) 번갈아
  pressure_gamma=100   필압 곡선 (100 = 그대로, 150이면 약하게, 70이면 강하게)
  tilt=1, invert_tilt=0 기울기 (브러시 모양이 반대로 기울면 invert_tilt=1)
  키 예: CTRL+Z, SHIFT+E, ALT+SPACE, F5, [, ]

단축키 (화면 창에서)
  R  화면 90° 회전
  C  색 범위 전환 (색이 물빠지거나 너무 진하면)

로그는 PadLinkRecv.log, 설정은 PadLinkRecv.ini 에 저장됩니다.
