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
