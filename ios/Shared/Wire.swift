import Foundation

/// PC와 주고받는 프레임 형식. 자세한 내용은 docs/protocol.md
///
/// 헤더 16바이트(리틀엔디언): kind u8, flags u8, reserved u16, length u32, pts_us u64
enum Wire {
    static let port: UInt16 = 47800
    /// 디버그용: 헤더 없는 H.264 Annex-B 스트림 (ffplay -f h264)
    static let rawVideoPort: UInt16 = 47801
    /// 디버그용: WAV 헤더 + s16le 스트림 (ffplay -f wav)
    static let rawAudioPort: UInt16 = 47802
    /// 펜 모드(앱이 화면에 떠 있을 때)의 입력 채널
    static let penPort: UInt16 = 47810
    static let bonjourType = "_padlink._tcp"
    static let protocolVersion = 1
    static let headerSize = 16

    enum Kind: UInt8 {
        /// JSON {"app","proto","role"}
        case hello = 0
        /// H.264 Annex-B 액세스 유닛. 키프레임이면 SPS/PPS 포함
        /// flags: bit0 = 키프레임, bit4-7 = CGImagePropertyOrientation(1~8)
        case videoFrame = 2
        /// sampleRate u32, channels u8, bits u8, reserved u16, 이후 s16le 인터리브
        case audioPCM = 3
        /// UTF-8 로그 한 줄
        case log = 4
        /// 펜 샘플 묶음: count u16, reserved u16, 이후 PenSample × count (각 32바이트)
        case penSamples = 16
        /// 펜 버튼: button u8(1 = 스퀴즈, 2 = 더블탭), phase u8(0 = 시작, 1 = 끝, 2 = 한 번)
        case penButton = 17
        /// PC → iPad: JSON {"width","height","name"} — 펜이 움직일 PC 화면 크기
        case penConfig = 32
        /// iPad → PC: 화면 디코딩을 이어가려면 키프레임이 필요함 (액정타블렛 모드)
        case keyframeRequest = 33
        /// JSON 제어: {"cmd":"stop"} 송출 멈춤, {"cmd":"close"} 펜 모드 닫기, {"bitrate":n} 송출 비트레이트
        case control = 48
    }

    enum PenPhase: UInt8 {
        case hover = 0, down = 1, move = 2, up = 3, hoverExit = 4, cancel = 5
    }

    enum PenButton: UInt8 {
        case squeeze = 1, doubleTap = 2
    }

    enum ButtonPhase: UInt8 {
        case began = 0, ended = 1, tap = 2
    }

    /// x, y는 활성 영역 기준 0~1, 각도는 라디안(UIKit 기준), z는 호버 높이 0~1
    struct PenSample {
        var phase: PenPhase
        var x: Float
        var y: Float
        var pressure: Float
        var altitude: Float
        var azimuth: Float
        var roll: Float
        var z: Float
    }

    static func penPayload(_ samples: [PenSample]) -> Data {
        var d = Data(capacity: 4 + samples.count * 32)
        d.appendLE(UInt16(samples.count))
        d.appendLE(UInt16(0))
        for s in samples {
            d.append(s.phase.rawValue)
            d.append(0)
            d.appendLE(UInt16(0))
            for v in [s.x, s.y, s.pressure, s.altitude, s.azimuth, s.roll, s.z] {
                d.appendLE(v.bitPattern)
            }
        }
        return d
    }

    static func buttonPayload(_ button: PenButton, _ phase: ButtonPhase) -> Data {
        Data([button.rawValue, phase.rawValue, 0, 0])
    }

    static func frame(_ kind: Kind, flags: UInt8 = 0, pts: UInt64, payload: Data) -> Data {
        var d = Data(capacity: headerSize + payload.count)
        d.append(kind.rawValue)
        d.append(flags)
        d.appendLE(UInt16(0))
        d.appendLE(UInt32(payload.count))
        d.appendLE(pts)
        d.append(payload)
        return d
    }

    static func helloPayload(role: String) -> Data {
        let info: [String: Any] = ["app": "PadLink", "proto": protocolVersion, "role": role]
        return (try? JSONSerialization.data(withJSONObject: info)) ?? Data()
    }

    static func audioPayload(pcm: Data, sampleRate: Int) -> Data {
        var d = Data(capacity: 8 + pcm.count)
        d.appendLE(UInt32(sampleRate))
        d.append(2)
        d.append(16)
        d.appendLE(UInt16(0))
        d.append(pcm)
        return d
    }

    /// 길이를 모르는 스트리밍용 WAV 헤더 (크기 필드는 최댓값)
    static func wavHeader(sampleRate: Int, channels: Int = 2) -> Data {
        var d = Data()
        d.append(contentsOf: Array("RIFF".utf8))
        d.appendLE(UInt32.max)
        d.append(contentsOf: Array("WAVEfmt ".utf8))
        d.appendLE(UInt32(16))
        d.appendLE(UInt16(1))
        d.appendLE(UInt16(channels))
        d.appendLE(UInt32(sampleRate))
        d.appendLE(UInt32(sampleRate * channels * 2))
        d.appendLE(UInt16(channels * 2))
        d.appendLE(UInt16(16))
        d.append(contentsOf: Array("data".utf8))
        d.appendLE(UInt32.max)
        return d
    }
}

extension Data {
    mutating func appendLE<T: FixedWidthInteger>(_ value: T) {
        var le = value.littleEndian
        Swift.withUnsafeBytes(of: &le) { append(contentsOf: $0) }
    }
}
