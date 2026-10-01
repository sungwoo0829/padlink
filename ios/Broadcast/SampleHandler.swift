import CoreMedia
import ReplayKit

/// 화면 방송 확장. iPad 전체 화면(다른 앱 포함)과 앱 소리를 PC로 보낸다.
final class SampleHandler: RPBroadcastSampleHandler {
    private let server = StreamServer(port: Wire.port, name: "main", bonjourName: "PadLink")
    private let rawVideo = StreamServer(port: Wire.rawVideoPort, name: "raw-video")
    private let rawAudio = StreamServer(port: Wire.rawAudioPort, name: "raw-audio")
    private let encoder = VideoEncoder()
    private let audio = AudioNormalizer()

    override func broadcastStarted(withSetupInfo setupInfo: [String: NSObject]?) {
        Log.sink = { [server] line in
            server.sendAll(Wire.frame(.log, pts: 0, payload: Data(line.utf8)))
        }

        encoder.onEncoded = { [server, rawVideo] annexB, isKey, pts, orientation in
            let flags: UInt8 = (isKey ? 1 : 0) | (orientation << 4)
            server.sendVideo(Wire.frame(.videoFrame, flags: flags, pts: pts, payload: annexB), isKey: isKey)
            rawVideo.sendVideo(annexB, isKey: isKey)
        }
        let wantKeyframe: () -> Void = { [encoder] in encoder.requestKeyframe() }
        server.onKeyframeNeeded = wantKeyframe
        rawVideo.onKeyframeNeeded = wantKeyframe

        server.makeGreeting = { Wire.frame(.hello, pts: 0, payload: Wire.helloPayload(role: "broadcast")) }
        rawAudio.makeGreeting = { [audio] in
            let rate = audio.sampleRate
            return Wire.wavHeader(sampleRate: rate > 0 ? rate : 48_000)
        }

        encoder.start()
        server.start()
        rawVideo.start()
        rawAudio.start()
        Log.write("방송 시작")
    }

    override func broadcastFinished() {
        Log.write("방송 종료")
        encoder.stop()
        server.stop()
        rawVideo.stop()
        rawAudio.stop()
        Log.sink = nil
    }

    override func processSampleBuffer(_ sampleBuffer: CMSampleBuffer, with sampleBufferType: RPSampleBufferType) {
        switch sampleBufferType {
        case .video:
            guard server.hasClients || rawVideo.hasClients,
                  let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
            let orientation = (CMGetAttachment(sampleBuffer, key: RPVideoSampleOrientationKey as CFString,
                                               attachmentModeOut: nil) as? NSNumber)?.uint8Value ?? 1
            encoder.encode(pixelBuffer, pts: CMSampleBufferGetPresentationTimeStamp(sampleBuffer),
                           orientation: orientation)

        case .audioApp:
            // WAV 헤더에 쓸 샘플레이트는 접속 전에도 알아 둔다
            if audio.noteFormat(sampleBuffer) {
                Log.write("오디오 샘플레이트 변경 \(audio.sampleRate)Hz, 원시 소리 접속을 끊음")
                rawAudio.disconnectAll()
            }
            guard server.hasClients || rawAudio.hasClients,
                  let pcm = audio.normalize(sampleBuffer) else { return }
            let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
            let micros = pts.isValid ? UInt64(max(0, pts.seconds) * 1_000_000) : 0
            server.sendAll(Wire.frame(.audioPCM, pts: micros,
                                      payload: Wire.audioPayload(pcm: pcm, sampleRate: audio.sampleRate)))
            rawAudio.sendAll(pcm)

        case .audioMic:
            break

        @unknown default:
            break
        }
    }
}
