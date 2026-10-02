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

기울기가 안 먹는 것 같으면
  pen-test.html 을 Chrome/Edge로 열고 펜 모드로 그려 보세요.
  "기울기 들어옴"이 뜨면 Windows까지는 정상이고, 클립 스튜디오 브러시가 기울기를 안 쓰는 것입니다.
  클립 스튜디오: 서브 도구 상세 → 브러시 크기(또는 브러시 끝의 방향) 옆 '영향 기준 설정'에서 '기울기'를 켜세요.
  기본 G펜 등은 필압만 씁니다.

액정타블렛 모드 (PC 화면을 iPad에 띄우기)
1. 새 모니터로 쓰려면 Virtual Display Driver(VDD, github.com/VirtualDrivers/Virtual-Display-Driver)를 설치하고
   해상도를 iPad Air 11"과 같은 2360x1640, 60Hz로 추가하세요. VDD 설정의 GPU는 RTX로.
   Windows 디스플레이 설정에서 '디스플레이 확장', 배율은 200%가 iPad와 비슷한 크기입니다.
   (VDD 없이 기존 모니터를 골라도 그 화면이 iPad에 복제됩니다)
2. 수신 앱 "펜 → 모니터"에서 "가상 모니터 (VDD) · 자동"을 고르고 "iPad에 화면 보내기"를 켭니다.
   가상 모니터는 iPad 펜 모드가 연결될 때 자동으로 켜지고, 끊긴 지 10초 뒤(또는 수신 앱을 끌 때) 꺼집니다.
   꺼지면 그 위에 있던 창은 Windows가 다른 모니터로 옮깁니다. 관리자 권한은 필요 없습니다.
   '자동'을 고르면 VDD는 PadLink가 관리합니다. 다른 모니터를 고르면 VDD는 건드리지 않습니다.
   켤 때 개수: PadLinkRecv.ini [display] vdd_count=1
3. iPad에서 펜 모드를 열면 그 화면이 펜 영역에 뜨고, 펜 위치가 화면과 1:1로 맞습니다.
   설정: PadLinkRecv.ini [display] bitrate_mbps=40, fps=60
   마우스 커서는 화면에 안 그려집니다 (펜 커서는 iPad에서 직접 그림).

단축키 (화면 창에서)
  R  화면 90° 회전
  C  색 범위 전환 (색이 물빠지거나 너무 진하면)

로그는 PadLinkRecv.log, 설정은 PadLinkRecv.ini 에 저장됩니다.
