import CoreMedia
import Foundation
import VideoToolbox

/// ReplayKit 화면을 하드웨어 H.264로 인코딩해서 Annex-B로 내보낸다.
/// 확장 메모리 한도(50MB) 때문에 버퍼를 쌓지 않고, B프레임 없이 바로바로 내보낸다.
final class VideoEncoder {
    /// (Annex-B, 키프레임 여부, pts 마이크로초, 방향) — VideoToolbox 내부 스레드에서 호출됨
    var onEncoded: ((Data, Bool, UInt64, UInt8) -> Void)?
    var bitrate = 20_000_000
    var frameRate = 60

    private static let startCode: [UInt8] = [0, 0, 0, 1]

    private let queue = DispatchQueue(label: "padlink.encoder")
    private var session: VTCompressionSession?
    private var width = 0
    private var height = 0
    private var keyframeRequested = false
    // ReplayKit은 화면이 바뀔 때만 프레임을 준다. 그림이 멈춰 있을 때 새로 접속한
    // PC가 키프레임을 못 받는 일이 없도록 마지막 프레임을 다시 인코딩한다.
    private var lastPixelBuffer: CVPixelBuffer?
    private var lastPTS = CMTime.invalid
    private var lastOrientation: UInt8 = 1
    private var lastEncodeUptime: TimeInterval = 0
    private var timer: DispatchSourceTimer?

    func start() {
        queue.async {
            let t = DispatchSource.makeTimerSource(queue: self.queue)
            t.schedule(deadline: .now() + 0.25, repeating: 0.25)
            t.setEventHandler { [weak self] in self?.repeatLastFrameIfIdle() }
            t.resume()
            self.timer = t
        }
    }

    func stop() {
        queue.sync {
            timer?.cancel()
            timer = nil
            lastPixelBuffer = nil
            teardownSession()
        }
    }

    func requestKeyframe() {
        queue.async { self.keyframeRequested = true }
    }

    func encode(_ pixelBuffer: CVPixelBuffer, pts: CMTime, orientation: UInt8) {
        queue.sync { encodeLocked(pixelBuffer, pts: pts, orientation: orientation) }
    }

    // MARK: - 인코더 큐 안에서만 호출

    private func repeatLastFrameIfIdle() {
        guard keyframeRequested, let pb = lastPixelBuffer, lastPTS.isValid else { return }
        let idle = ProcessInfo.processInfo.systemUptime - lastEncodeUptime
        guard idle > 0.2 else { return }
        let pts = CMTimeAdd(lastPTS, CMTime(seconds: idle, preferredTimescale: 1_000_000))
        encodeLocked(pb, pts: pts, orientation: lastOrientation)
    }

    private func encodeLocked(_ pixelBuffer: CVPixelBuffer, pts: CMTime, orientation: UInt8) {
        let w = CVPixelBufferGetWidth(pixelBuffer)
        let h = CVPixelBufferGetHeight(pixelBuffer)
        if session == nil || w != width || h != height {
            guard makeSession(width: w, height: h) else { return }
            keyframeRequested = true
        }
        guard let session else { return }

        var frameProperties: CFDictionary?
        if keyframeRequested {
            frameProperties = [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary
            keyframeRequested = false
        }
        lastPixelBuffer = pixelBuffer
        lastPTS = pts
        lastOrientation = orientation
        lastEncodeUptime = ProcessInfo.processInfo.systemUptime

        let status = VTCompressionSessionEncodeFrame(
            session,
            imageBuffer: pixelBuffer,
            presentationTimeStamp: pts,
            duration: .invalid,
            frameProperties: frameProperties,
            infoFlagsOut: nil
        ) { [weak self] status, _, sample in
            guard status == noErr, let sample, let self else { return }
            self.emit(sample, orientation: orientation)
        }
        if status != noErr { Log.write("인코딩 실패 \(status)") }
    }

    private func makeSession(width: Int, height: Int) -> Bool {
        teardownSession()
        var created: VTCompressionSession?
        let lowLatency = [kVTVideoEncoderSpecification_EnableLowLatencyRateControl as String: true] as CFDictionary
        var status = VTCompressionSessionCreate(
            allocator: nil, width: Int32(width), height: Int32(height),
            codecType: kCMVideoCodecType_H264, encoderSpecification: lowLatency,
            imageBufferAttributes: nil, compressedDataAllocator: nil,
            outputCallback: nil, refcon: nil, compressionSessionOut: &created)
        if status != noErr {
            Log.write("저지연 인코더 생성 실패 \(status), 일반 모드로 재시도")
            status = VTCompressionSessionCreate(
                allocator: nil, width: Int32(width), height: Int32(height),
                codecType: kCMVideoCodecType_H264, encoderSpecification: nil,
                imageBufferAttributes: nil, compressedDataAllocator: nil,
                outputCallback: nil, refcon: nil, compressionSessionOut: &created)
        }
        guard status == noErr, let s = created else {
            Log.write("인코더 생성 실패 \(status)")
            return false
        }

        func set(_ key: CFString, _ value: CFTypeRef) {
            let r = VTSessionSetProperty(s, key: key, value: value)
            if r != noErr { Log.write("인코더 속성 \(key) 설정 실패 \(r)") }
        }
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel)
        set(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: bitrate))
        set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: frameRate))
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, NSNumber(value: 2))
        VTCompressionSessionPrepareToEncodeFrames(s)

        session = s
        self.width = width
        self.height = height
        Log.write("인코더 시작 \(width)x\(height) \(bitrate / 1_000_000)Mbps")
        return true
    }

    private func teardownSession() {
        guard let session else { return }
        VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
        VTCompressionSessionInvalidate(session)
        self.session = nil
    }

    /// AVCC(길이 접두) → Annex-B(시작 코드). 키프레임 앞에는 SPS/PPS를 붙인다.
    private func emit(_ sample: CMSampleBuffer, orientation: UInt8) {
        guard let format = CMSampleBufferGetFormatDescription(sample),
              let block = CMSampleBufferGetDataBuffer(sample) else { return }

        let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[String: Any]]
        let notSync = attachments?.first?[kCMSampleAttachmentKey_NotSync as String] as? Bool ?? false
        let isKey = !notSync

        var parameterSetCount = 0
        var nalHeaderLength: Int32 = 4
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            format, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil,
            parameterSetCountOut: &parameterSetCount, nalUnitHeaderLengthOut: &nalHeaderLength)

        var out = Data()
        if isKey {
            for i in 0..<parameterSetCount {
                var pointer: UnsafePointer<UInt8>?
                var size = 0
                let r = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    format, parameterSetIndex: i, parameterSetPointerOut: &pointer, parameterSetSizeOut: &size,
                    parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
                if r == noErr, let pointer {
                    out.append(contentsOf: Self.startCode)
                    out.append(pointer, count: size)
                }
            }
        }

        let total = CMBlockBufferGetDataLength(block)
        var avcc = [UInt8](repeating: 0, count: total)
        guard CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: total, destination: &avcc) == noErr else { return }
        let n = Int(nalHeaderLength)
        var i = 0
        while i + n <= total {
            var length = 0
            for k in 0..<n { length = (length << 8) | Int(avcc[i + k]) }
            i += n
            guard length > 0, i + length <= total else { break }
            out.append(contentsOf: Self.startCode)
            out.append(contentsOf: avcc[i..<(i + length)])
            i += length
        }

        let pts = CMSampleBufferGetPresentationTimeStamp(sample)
        let micros = pts.isValid ? UInt64(max(0, pts.seconds) * 1_000_000) : 0
        onEncoded?(out, isKey, micros, orientation)
    }
}
