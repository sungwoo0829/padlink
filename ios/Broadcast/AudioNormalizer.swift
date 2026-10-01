import AudioToolbox
import CoreMedia
import Foundation

/// ReplayKit 앱 오디오를 s16le 스테레오 인터리브로 맞춘다.
/// 포맷이 기기·iOS 버전마다 달라서(빅엔디언 Int16이 오기도 함) ASBD를 보고 직접 변환한다.
/// 샘플레이트 변환은 PC(FMOD)가 하므로 원래 레이트를 그대로 둔다.
final class AudioNormalizer {
    private let lock = NSLock()
    private var _sampleRate = 0
    private var warnedFormat = false

    /// 마지막으로 본 샘플레이트 (아직 모르면 0)
    var sampleRate: Int {
        lock.lock()
        defer { lock.unlock() }
        return _sampleRate
    }

    /// 포맷만 읽어서 샘플레이트를 갱신한다. 이전과 달라졌으면 true
    @discardableResult
    func noteFormat(_ sampleBuffer: CMSampleBuffer) -> Bool {
        guard let asbd = Self.streamDescription(sampleBuffer) else { return false }
        let rate = Int(asbd.mSampleRate)
        lock.lock()
        defer { lock.unlock() }
        let changed = _sampleRate != 0 && _sampleRate != rate
        _sampleRate = rate
        return changed
    }

    func normalize(_ sampleBuffer: CMSampleBuffer) -> Data? {
        guard let asbd = Self.streamDescription(sampleBuffer), asbd.mFormatID == kAudioFormatLinearPCM else { return nil }
        let frames = CMSampleBufferGetNumSamples(sampleBuffer)
        let channels = Int(asbd.mChannelsPerFrame)
        guard frames > 0, channels > 0 else { return nil }

        let flags = asbd.mFormatFlags
        let isFloat = flags & kAudioFormatFlagIsFloat != 0
        let bigEndian = flags & kAudioFormatFlagIsBigEndian != 0
        let nonInterleaved = flags & kAudioFormatFlagIsNonInterleaved != 0
        let bits = Int(asbd.mBitsPerChannel)
        let supported = (isFloat && bits == 32) || (!isFloat && (bits == 16 || bits == 32))
        guard supported else {
            if !warnedFormat {
                warnedFormat = true
                Log.write("지원하지 않는 오디오 포맷: flags=\(flags) bits=\(bits) ch=\(channels)")
            }
            return nil
        }

        let bufferCount = nonInterleaved ? channels : 1
        let list = AudioBufferList.allocate(maximumBuffers: bufferCount)
        defer { free(list.unsafeMutablePointer) }
        var block: CMBlockBuffer?
        let status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer,
            bufferListSizeNeededOut: nil,
            bufferListOut: list.unsafeMutablePointer,
            bufferListSize: AudioBufferList.sizeInBytes(maximumBuffers: bufferCount),
            blockBufferAllocator: nil,
            blockBufferMemoryAllocator: nil,
            flags: kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
            blockBufferOut: &block)
        guard status == noErr else { return nil }

        let bytesPerSample = bits / 8
        func sample(_ frame: Int, _ channel: Int) -> Int16 {
            let buffer = list[nonInterleaved ? channel : 0]
            guard let base = buffer.mData else { return 0 }
            let index = nonInterleaved ? frame : frame * channels + channel
            let offset = index * bytesPerSample
            guard offset + bytesPerSample <= Int(buffer.mDataByteSize) else { return 0 }
            let p = UnsafeRawPointer(base).advanced(by: offset)
            if isFloat {
                var raw = p.loadUnaligned(as: UInt32.self)
                if bigEndian { raw = raw.byteSwapped }
                let v = Float(bitPattern: raw)
                guard v.isFinite else { return 0 }
                return Int16(max(-1, min(1, v)) * 32767)
            } else if bits == 16 {
                var raw = p.loadUnaligned(as: UInt16.self)
                if bigEndian { raw = raw.byteSwapped }
                return Int16(bitPattern: raw)
            } else {
                var raw = p.loadUnaligned(as: UInt32.self)
                if bigEndian { raw = raw.byteSwapped }
                return Int16(truncatingIfNeeded: Int32(bitPattern: raw) >> 16)
            }
        }

        var out = [Int16](repeating: 0, count: frames * 2)
        for f in 0..<frames {
            let left = sample(f, 0)
            let right = channels > 1 ? sample(f, 1) : left
            out[f * 2] = left.littleEndian
            out[f * 2 + 1] = right.littleEndian
        }
        return out.withUnsafeBytes { Data($0) }
    }

    private static func streamDescription(_ sampleBuffer: CMSampleBuffer) -> AudioStreamBasicDescription? {
        guard let format = CMSampleBufferGetFormatDescription(sampleBuffer),
              let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(format) else { return nil }
        return asbd.pointee
    }
}
