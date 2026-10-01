# PadLink 프로토콜 (v1)

iPad가 TCP 서버, PC가 클라이언트. USB는 usbmuxd(Windows: Apple Devices 앱의
Apple Mobile Device Service, `127.0.0.1:27015`)로 기기 포트에 연결하고,
Wi-Fi는 iPad IP로 바로 연결한다. Bonjour 서비스 타입은 `_padlink._tcp`.

| 포트 | 내용 |
|---|---|
| 47800 | 아래 프레임 프로토콜 |
| 47801 | 디버그: 헤더 없는 H.264 Annex-B (`ffplay -f h264`) |
| 47802 | 디버그: 스트리밍 WAV 헤더 + s16le (`ffplay -f wav`) |

## 프레임

헤더 16바이트, 모두 리틀엔디언.

| 오프셋 | 크기 | 필드 |
|---|---|---|
| 0 | 1 | kind |
| 1 | 1 | flags |
| 2 | 2 | reserved (0) |
| 4 | 4 | payload 길이 |
| 8 | 8 | pts (마이크로초, iPad 호스트 시계) |

| kind | 이름 | payload |
|---|---|---|
| 0 | hello | JSON `{"app":"PadLink","proto":1,"role":"broadcast"}` — 접속 직후 첫 프레임 |
| 2 | videoFrame | H.264 Annex-B 액세스 유닛. 키프레임이면 SPS/PPS 포함. flags bit0 = 키프레임, bit4–7 = CGImagePropertyOrientation(1~8) |
| 3 | audioPCM | sampleRate u32, channels u8(=2), bits u8(=16), reserved u16, 이후 s16le 인터리브 |
| 4 | log | UTF-8 한 줄 |

- 접속 직후 영상은 키프레임부터 온다. 화면이 멈춰 있어도 0.2초 안에 마지막 화면으로 키프레임을 만든다.
- 네트워크가 밀리면(4MB 초과) 영상은 다음 키프레임까지 건너뛰고, 소리는 그 조각을 버린다.
- 오디오 샘플레이트는 iPad가 주는 그대로(보통 44.1k 또는 48k). PC에서 리샘플한다.
