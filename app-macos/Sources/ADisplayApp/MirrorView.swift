// ADisplay —— macOS 的 AirPlay 镜像显示面
//
// 用 AVSampleBufferDisplayLayer：它直接吃**压缩**的 H.264/H.265，自己负责硬解
// 与显示，延迟最低。这跟「解码渲染交给各平台自带的实现」这条一贯的分工一致 ——
// DLNA 那边交给 AVPlayer，镜像这边交给这个层，核心里不养解码器。
//
// 格式上有一个必须注意的落差，弄反了就是「一帧都解不出来」：
//
//   核心交过来的是 **Annex B** —— 每个 NALU 前面是 00 00 01 或 00 00 00 01
//   起始码（这一点是从协议层源码确认的：它给每个 NALU 写的就是起始码）。
//   而 AVSampleBufferDisplayLayer 要的是 **AVCC** —— 4 字节大端长度前缀，
//   配合 nalUnitHeaderLength = 4 的格式描述。
//
// 所以这里要先按起始码切开、重新按长度前缀拼一遍，再入队。
// 起始码那一版是能「看着像在跑」的：帧照样计数、切出来的东西也能入队，
// 只是解码器一个 NALU 都认不出来，屏幕上什么都没有。

import AVFoundation
import AppKit
import SwiftUI

/// 镜像帧的落点。由渲染面实现，核心每来一帧调一次。
protocol MirrorFrameSink: AnyObject {
    /// data 只在本次调用期间有效 —— 需要留存必须自己拷走。
    func enqueueMirrorFrame(_ data: UnsafePointer<UInt8>, count: Int, isH265: Bool)
}

/// 镜像帧的转接处。
///
/// 刻意不做成 EngineModel 的一部分：那个类是 @MainActor 的，而每帧都往主线程跳
/// 一次会把解码队列压满、平白多出一大截延迟。这里只在锁内取一次指针就返回，
/// 剩下的（解析、建样例、入队）都在调用线程上做完 —— AVSampleBufferDisplayLayer
/// 本来就是线程安全的，这正是它被设计出来的用法。
final class MirrorFrameRouter {

    static let shared = MirrorFrameRouter()

    private let lock = NSLock()
    // weak：渲染面被拆掉之后就不该再有帧往里送。取出来的局部变量是强引用，
    // 所以调用期间它不会被释放。
    private weak var sink: MirrorFrameSink?

    private init() {}

    func attach(_ sink: MirrorFrameSink?) {
        lock.lock()
        self.sink = sink
        lock.unlock()
    }

    func deliver(_ data: UnsafePointer<UInt8>, count: Int, isH265: Bool) {
        lock.lock()
        let target = sink
        lock.unlock()
        target?.enqueueMirrorFrame(data, count: count, isH265: isH265)
    }
}

/// 真正干活的视图：背层就是显示层。
final class MirrorRenderView: NSView, MirrorFrameSink {

    private let displayLayer = AVSampleBufferDisplayLayer()
    private var formatDescription: CMVideoFormatDescription?
    private var lastPresentationTime = CMTime.invalid

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
    }

    required init?(coder: NSCoder) {
        nil   // 这个视图只由代码创建
    }

    override func makeBackingLayer() -> CALayer {
        displayLayer.videoGravity = .resizeAspect
        // 黑底：画面比例与窗口不一致时留出来的是黑边而不是主题色的一块。
        displayLayer.backgroundColor = NSColor.black.cgColor
        return displayLayer
    }

    // MARK: - 收帧

    func enqueueMirrorFrame(_ data: UnsafePointer<UInt8>, count: Int, isH265: Bool) {
        guard count > 4 else { return }

        let nalUnits = MirrorRenderView.splitAnnexB(data, count: count)
        guard !nalUnits.isEmpty else { return }

        // 参数集可以夹在任何一帧里（切分辨率、切编码器时会重发），
        // 所以每帧都扫一遍，见到就重建格式描述。
        if let sets = MirrorRenderView.parameterSets(in: nalUnits, isH265: isH265) {
            rebuildFormatDescription(with: sets, isH265: isH265)
        }
        guard let format = formatDescription else {
            return   // 还没拿到参数集，这一帧解不了，丢掉即可
        }

        // 解码器卡住时（比如刚切过分辨率）要主动冲一次，否则它会一直拒绝新帧。
        if displayLayer.requiresFlushToResumeDecoding {
            displayLayer.flush()
        }
        guard displayLayer.isReadyForMoreMediaData else {
            return   // 来不及就丢帧：镜像宁可按最新画面走，也不要越积越久
        }

        let avcc = MirrorRenderView.annexBToAVCC(nalUnits)
        guard !avcc.isEmpty, let sample = makeSampleBuffer(avcc: avcc, format: format) else {
            return
        }
        displayLayer.enqueue(sample)
    }

    private func rebuildFormatDescription(with sets: [[UInt8]], isH265: Bool) {
        var pointers: [UnsafePointer<UInt8>] = []
        var sizes: [Int] = []
        // 这些数组元素的生存期只到本次调用结束，而创建函数会把内容拷走，
        // 所以用 withUnsafeBufferPointer 逐层嵌套是安全的。
        for set in sets {
            set.withUnsafeBufferPointer { buffer in
                if let base = buffer.baseAddress {
                    pointers.append(base)
                    sizes.append(buffer.count)
                }
            }
        }
        guard pointers.count == sets.count, !pointers.isEmpty else { return }

        var format: CMVideoFormatDescription?
        let status: OSStatus
        if isH265 {
            status = pointers.withUnsafeBufferPointer { pointerBuffer in
                sizes.withUnsafeBufferPointer { sizeBuffer in
                    CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                        allocator: kCFAllocatorDefault,
                        parameterSetCount: sets.count,
                        parameterSetPointers: pointerBuffer.baseAddress!,
                        parameterSetSizes: sizeBuffer.baseAddress!,
                        nalUnitHeaderLength: 4,
                        extensions: nil,
                        formatDescriptionOut: &format)
                }
            }
        } else {
            status = pointers.withUnsafeBufferPointer { pointerBuffer in
                sizes.withUnsafeBufferPointer { sizeBuffer in
                    CMVideoFormatDescriptionCreateFromH264ParameterSets(
                        allocator: kCFAllocatorDefault,
                        parameterSetCount: sets.count,
                        parameterSetPointers: pointerBuffer.baseAddress!,
                        parameterSetSizes: sizeBuffer.baseAddress!,
                        nalUnitHeaderLength: 4,
                        formatDescriptionOut: &format)
                }
            }
        }

        if status == noErr, let created = format {
            if formatDescription == nil {
                let dims = CMVideoFormatDescriptionGetDimensions(created)
                NSLog("ADisplay: 镜像编码参数已就绪，%dx%d", dims.width, dims.height)
            }
            formatDescription = created
        }
    }

    private func makeSampleBuffer(avcc: [UInt8], format: CMVideoFormatDescription) -> CMSampleBuffer? {
        var blockBuffer: CMBlockBuffer?
        // 这里刻意不用「零拷贝」那个重载：数据来自核心的缓冲区，只在回调期间有效，
        // 而入队之后解码器还要用它。所以老老实实拷一份。
        let created = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault,
            memoryBlock: nil,
            blockLength: avcc.count,
            blockAllocator: kCFAllocatorDefault,
            customBlockSource: nil,
            offsetToData: 0,
            dataLength: avcc.count,
            flags: 0,
            blockBufferOut: &blockBuffer)
        guard created == kCMBlockBufferNoErr, let block = blockBuffer else { return nil }

        let copied = avcc.withUnsafeBytes { raw -> OSStatus in
            guard let base = raw.baseAddress else { return -1 }
            return CMBlockBufferReplaceDataBytes(
                with: base, blockBuffer: block, offsetIntoDestination: 0, dataLength: avcc.count)
        }
        guard copied == kCMBlockBufferNoErr else { return nil }

        // 显示时间戳要严格递增：同一时刻两帧会让显示层丢掉后一帧。
        // 按 60fps 递推，够用了 —— 镜像的帧率就是跟着发送端走的。
        let pts = nextPresentationTime()
        var timing = CMSampleTimingInfo(
            duration: CMTime(value: 1, timescale: 60),
            presentationTimeStamp: pts,
            decodeTimeStamp: .invalid)
        var sampleSize = avcc.count

        var sampleBuffer: CMSampleBuffer?
        let status = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault,
            dataBuffer: block,
            formatDescription: format,
            sampleCount: 1,
            sampleTimingEntryCount: 1,
            sampleTimingArray: &timing,
            sampleSizeEntryCount: 1,
            sampleSizeArray: &sampleSize,
            sampleBufferOut: &sampleBuffer)
        guard status == noErr, let sample = sampleBuffer else { return nil }

        // 非关键帧要标出来，否则解码器会把每一帧都当随机访问点，画面会花。
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true),
           CFArrayGetCount(attachments) > 0 {
            let dict = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(dict,
                                 Unmanaged.passUnretained(kCMSampleAttachmentKey_NotSync).toOpaque(),
                                 Unmanaged.passUnretained(kCFBooleanFalse).toOpaque())
        }
        return sample
    }

    /// 上一帧的时间加一档，保证严格递增。
    private func nextPresentationTime() -> CMTime {
        let step = CMTime(value: 1, timescale: 60)
        if lastPresentationTime.isValid {
            lastPresentationTime = lastPresentationTime + step
        } else {
            lastPresentationTime = CMTime(value: 0, timescale: 60)
        }
        return lastPresentationTime
    }

    // MARK: - AVCC 解析

    /// 按起始码切开一帧 Annex B。
    static func splitAnnexB(_ data: UnsafePointer<UInt8>, count: Int) -> [[UInt8]] {
        // 先记下每个起始码的位置：prefix 是起始码本身的下标，content 是它后面
        // 第一个字节。一段 NALU 的结束就是下一个起始码的 prefix ——
        // 这样 4 字节起始码前面那些多余的 0 会被自动排除在外。
        var marks: [(prefix: Int, content: Int)] = []
        var index = 0
        while index + 3 <= count {
            if data[index] == 0 && data[index + 1] == 0 {
                if data[index + 2] == 1 {
                    marks.append((prefix: index, content: index + 3))
                    index += 3
                    continue
                }
                if index + 4 <= count && data[index + 2] == 0 && data[index + 3] == 1 {
                    marks.append((prefix: index, content: index + 4))
                    index += 4
                    continue
                }
            }
            index += 1
        }
        guard !marks.isEmpty else { return [] }

        var units: [[UInt8]] = []
        for (position, mark) in marks.enumerated() {
            var end = position + 1 < marks.count ? marks[position + 1].prefix : count
            // 末尾可能还挂着几个 0（上一段 NALU 的填充），去掉再收。
            while end > mark.content && data[end - 1] == 0 {
                end -= 1
            }
            guard end > mark.content else { continue }
            units.append(Array(UnsafeBufferPointer(start: data + mark.content, count: end - mark.content)))
        }
        return units
    }

    /// Annex B 的 NALU 拼成 AVCC：每个前面换成 4 字节大端长度。
    static func annexBToAVCC(_ units: [[UInt8]]) -> [UInt8] {
        var out: [UInt8] = []
        for unit in units {
            let length = UInt32(unit.count)
            out.append(UInt8((length >> 24) & 0xFF))
            out.append(UInt8((length >> 16) & 0xFF))
            out.append(UInt8((length >> 8) & 0xFF))
            out.append(UInt8(length & 0xFF))
            out.append(contentsOf: unit)
        }
        return out
    }

    /// 从这一帧切出来的 NALU 里挑出参数集。没有就返回 nil。
    static func parameterSets(in units: [[UInt8]], isH265: Bool) -> [[UInt8]]? {
        // H.264：SPS=7、PPS=8；H.265：VPS=32、SPS=33、PPS=34。
        // 类型在第一个字节的低 5 位（H.264）或 (byte >> 1) & 0x3F（H.265）。
        var sets: [[UInt8]] = []
        for unit in units where !unit.isEmpty {
            let type = isH265 ? (Int(unit[0]) >> 1) & 0x3F : Int(unit[0]) & 0x1F
            let wanted = isH265 ? (type == 32 || type == 33 || type == 34)
                                : (type == 7 || type == 8)
            if wanted {
                sets.append(unit)
            }
        }
        if sets.isEmpty {
            return nil
        }
        // 只给 SPS 不给 PPS 是残缺的，建不出格式描述 —— 等下一帧凑齐再说。
        let needed = isH265 ? 3 : 2
        return sets.count >= needed ? sets : nil
    }
}

/// SwiftUI 侧的包装。
struct MirrorSurface: NSViewRepresentable {

    func makeNSView(context: Context) -> MirrorRenderView {
        let view = MirrorRenderView(frame: .zero)
        MirrorFrameRouter.shared.attach(view)
        return view
    }

    func updateNSView(_ nsView: MirrorRenderView, context: Context) {
        // 视图会被复用，而转接处只认一个落点 —— 每次更新时重新认一次，
        // 保证帧进的是当前这块。
        MirrorFrameRouter.shared.attach(nsView)
    }

    static func dismantleNSView(_ nsView: MirrorRenderView, coordinator: ()) {
        // 拆掉之后就不该再有帧送进来，否则会对着一块没有窗口的层入队。
        MirrorFrameRouter.shared.attach(nil)
    }
}
