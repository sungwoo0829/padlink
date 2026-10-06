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
1. Virtual Display Driver(VDD, github.com/VirtualDrivers/Virtual-Display-Driver)를 설치하고
   해상도를 iPad Air 11"과 같은 2360x1640, 60Hz로 추가하세요. VDD 설정의 GPU는 RTX로.
   Windows 디스플레이 설정에서 '디스플레이 확장', 배율은 200%가 iPad와 비슷한 크기입니다.
2. 수신 앱 "펜 → 모니터"에서 "가상 모니터 (VDD)"를 고르세요. (화면 보내기는 자동으로 켜짐)
   - 가상 모니터는 평소에는 꺼 둡니다. iPad에서 펜 모드를 열면 켜지고, 닫은 지 5초 뒤 꺼집니다.
   - 끄고 켜기는 설정 앱의 '이 디스플레이 연결 끊기'와 같은 방식이라 관리자 권한이 필요 없고,
     꺼질 때 그 위의 창은 Windows가 다른 모니터로 옮깁니다.
   - 다른 모니터를 고르면 그 화면을 iPad에 복제하고, VDD는 건드리지 않습니다.
3. 화질: "액정타블렛 화질" 칸(Mbps)을 바꾸고 [적용]. HEVC, 60fps 고정입니다.
   화면이 멈춰 있어도 60fps로 보냅니다. 화질이 부족하면 80~120으로 올려 보세요.
   (PadLinkRecv.ini [display] codec=h264 로 H.264로 바꿀 수 있음)
4. iPad에서 펜 모드와 송출(방송)은 하나만 켜집니다. 펜 모드를 열면 송출이 멈추고,
   송출을 시작하면 펜 모드가 닫힙니다. (USB 대역을 나눠 쓰지 않도록)
   송출 화질은 "송출 화질" 칸에서 바꿉니다(바로 적용).
   마우스 커서는 화면에 안 그려집니다 (펜 커서는 iPad에서 직접 그림).

'PadLink 화면' 창 (iPad 송출 화면)
  우클릭 메뉴에 아래 기능이 모두 있습니다.
  Ctrl+C       복사하기 — 창 크기·검은 여백과 상관없이 iPad 화면을 원본 해상도(PNG)로 클립보드에
  B / 더블클릭  테두리 없이 — 제목 표시줄·테두리가 없어져 스크린샷·디스코드 창 공유가 깔끔해짐.
               켤 때 영상 비율에 맞춰 검은 여백도 없앰. 아무 데나 끌어서 옮기고 가장자리로 크기 조절
  F            영상 비율에 맞추기 (검은 여백 없애기)
  R            화면 90° 회전
  C            색 범위 전환 (색이 물빠지거나 너무 진하면)
  창 위치·크기·테두리 상태는 다음에 켤 때도 그대로입니다.

로그는 PadLinkRecv.log, 설정은 PadLinkRecv.ini 에 저장됩니다.
