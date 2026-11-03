PadLink 수신 (우분투 24.04)
===========================

설치
  sudo apt install ./padlink-recv_*_amd64.deb
  (FFmpeg 라이브러리, usbmuxd, 한글 폰트가 같이 깔립니다)
  앱 메뉴의 "PadLink 수신" 또는 터미널에서 padlink-recv

USB 연결
  iPad를 케이블로 연결하고 iPad에서 "이 컴퓨터 신뢰"를 누르세요.
  (usbmuxd가 알아서 짝을 맞춥니다. 안 되면 sudo systemctl restart usbmuxd)

사용
  1. iPad에서 PadLink → 방송 버튼 → "PadLink 송출"
  2. "PadLink 수신" → USB 또는 Wi-Fi(IP 입력) → 연결
  3. 소리 "출력 장치"에서 오디오 인터페이스(Focusrite 등)를 고르세요.
  4. 디스코드에서는 "PadLink 화면" 창을 공유하세요. 소리도 같이 나갑니다.
     (공식 디스코드 0.0.76 이상. Windows판과 달리 소리는 한 갈래로 충분합니다:
      리눅스 디스코드는 이 앱의 소리를 PipeWire에서 그대로 잡아갑니다.)

'PadLink 화면' 창 (우클릭 메뉴에 모두 있음)
  Ctrl+C       복사하기 — 창 크기·검은 여백과 상관없이 iPad 화면을 원본 해상도 PNG로 클립보드에
  B / 더블클릭  테두리 없이 — 스크린샷·디스코드 공유가 깔끔해짐. 켤 때 검은 여백도 없앰.
               테두리가 없을 때는 아무 데나 끌어서 옮기고 가장자리로 크기 조절 (되돌리기: B 또는 우클릭)
  F            영상 비율에 맞추기
  R            90° 회전
  C            색 범위 전환

설정·로그
  ~/.config/padlink/padlink-recv.conf
  ~/.config/padlink/padlink-recv.log   ← 문제가 있으면 이 파일을 보내 주세요
