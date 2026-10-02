import AVFoundation
import CoreMedia
import UIKit

/// PC 화면(H.264 Annex-B)을 AVSampleBufferDisplayLayer로 바로 띄운다 (액정타블렛 모드)
final class VideoReceiver {
    let layer = AVSampleBufferDisplayLayer()
    /// 디코딩을 이어가려면 키프레임이 필요할 때 (내부 큐에서 호출됨)
    var onNeedKeyframe: (() -> Void)?

    private let queue = DispatchQueue(label: "padlink.video-receiver")
    private let lock = NSLock()
    private var lastFrameUptime: TimeInterval = 0
    private var format: CMVideoFormatDescription?
    private var sps = Data()
    private var pps = Data()
    private var formatSPS = Data()
    private var formatPPS = Data()
    private var waitingForKey = true
    private var lastKeyRequest: TimeInterval = 0

    init() {
        layer.videoGravity = .resizeAspect
        layer.backgroundColor = UIColor.black.cgColor
    }

    /// 최근 2초 안에 화면을 받았는지
    var isReceiving: Bool {
        lock.lock()
        defer { lock.unlock() }
        return ProcessInfo.processInfo.systemUptime - lastFrameUptime < 2
    }

    func submit(_ annexB: Data) {
        queue.async { self.handle(annexB) }
    }

    func clear() {
        queue.async {
            self.layer.sampleBufferRenderer.flush(removingDisplayedImage: true, completionHandler: nil)
            self.waitingForKey = true
            self.lock.lock()
            self.lastFrameUptime = 0
            self.lock.unlock()
        }
    }

    // MARK: - 내부 큐

    private func handle(_ annexB: Data) {
        var isKey = false
        var units: [Data] = []
        for nal in Self.split(annexB) {
            guard let header = nal.first else { continue }
            switch header & 0x1F {
            case 7: sps = nal
            case 8: pps = nal
            case 9: break  // AUD
            case 5:
                isKey = true
                units.append(nal)
            default:
                units.append(nal)
            }
        }
        if isKey { rebuildFormatIfNeeded() }
        if waitingForKey {
            guard isKey, format != nil else {
                requestKeyframe()
                return
            }
            waitingForKey = false
        }
        guard let format, !units.isEmpty else { return }

        let renderer = layer.sampleBufferRenderer
        if renderer.status == .failed || renderer.requiresFlushToResumeDecoding {
            renderer.flush()
            if !isKey {
                waitingForKey = true
                requestKeyframe()
                return
            }
        }
        guard let sample = Self.makeSample(units, format: format) else { return }
        renderer.enqueue(sample)
        lock.lock()
        lastFrameUptime = ProcessInfo.processInfo.systemUptime
        lock.unlock()
    }

    private func requestKeyframe() {
        let now = ProcessInfo.processInfo.systemUptime
        guard now - lastKeyRequest > 0.5 else { return }
        lastKeyRequest = now
        onNeedKeyframe?()
    }

    private func rebuildFormatIfNeeded() {
        guard !sps.isEmpty, !pps.isEmpty, format == nil || sps != formatSPS || pps != formatPPS else { return }
        var created: CMFormatDescription?
        let status = sps.withUnsafeBytes { s in
            pps.withUnsafeBytes { p -> OSStatus in
                let pointers = [s.bindMemory(to: UInt8.self).baseAddress!, p.bindMemory(to: UInt8.self).baseAddress!]
                let sizes = [s.count, p.count]
                return CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    allocator: kCFAllocatorDefault, parameterSetCount: 2, parameterSetPointers: pointers,
                    parameterSetSizes: sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &created)
            }
        }
        guard status == noErr, let created else {
            Log.write("PC 화면 형식을 만들 수 없음 \(status)")
            return
        }
        format = created
        formatSPS = sps
        formatPPS = pps
        let dims = CMVideoFormatDescriptionGetDimensions(created)
        Log.write("PC 화면 수신 \(dims.width)x\(dims.height)")
    }

    /// 시작 코드(00 00 01 / 00 00 00 01)로 NAL 단위를 나눈다
    static func split(_ data: Data) -> [Data] {
        var result: [Data] = []
        data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            let b = raw.bindMemory(to: UInt8.self)
            guard let base = b.baseAddress else { return }
            let n = b.count
            var i = 0
            var start = -1
            while i + 2 < n {
                if b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 {
                    if start >= 0 {
                        var end = i
                        if end > start && b[end - 1] == 0 { end -= 1 }
                        if end > start { result.append(Data(bytes: base + start, count: end - start)) }
                    }
                    i += 3
                    start = i
                } else {
                    i += 1
                }
            }
            if start >= 0 && start < n { result.append(Data(bytes: base + start, count: n - start)) }
        }
        return result
    }

    /// NAL 단위들 → 길이 접두(AVCC) 샘플. 받는 즉시 표시하도록 표시한다.
    static func makeSample(_ units: [Data], format: CMVideoFormatDescription) -> CMSampleBuffer? {
        var avcc = Data(capacity: units.reduce(0) { $0 + $1.count + 4 })
        for unit in units {
            var length = UInt32(unit.count).bigEndian
            withUnsafeBytes(of: &length) { avcc.append(contentsOf: $0) }
            avcc.append(unit)
        }
        var block: CMBlockBuffer?
        guard CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: avcc.count, blockAllocator: kCFAllocatorDefault,
            customBlockSource: nil, offsetToData: 0, dataLength: avcc.count, flags: 0, blockBufferOut: &block
        ) == kCMBlockBufferNoErr, let block else { return nil }
        let copied = avcc.withUnsafeBytes {
            CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block, offsetIntoDestination: 0,
                                          dataLength: avcc.count)
        }
        guard copied == kCMBlockBufferNoErr else { return nil }

        var sample: CMSampleBuffer?
        var size = avcc.count
        guard CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault, dataBuffer: block, formatDescription: format, sampleCount: 1,
            sampleTimingEntryCount: 0, sampleTimingArray: nil, sampleSizeEntryCount: 1, sampleSizeArray: &size,
            sampleBufferOut: &sample
        ) == noErr, let sample else { return nil }
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true),
           CFArrayGetCount(attachments) > 0 {
            let dict = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(dict, Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                                 Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }
        return sample
    }
}
