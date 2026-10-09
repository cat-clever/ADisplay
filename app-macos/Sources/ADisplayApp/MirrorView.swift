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
//
// 另一条约定同样重要：**参数集（SPS/PPS）一定会随帧一起来，而且不只第一帧有**。
// 协议层原本只在第一帧上挂一次，而渲染面的挂载要等主线程转一圈，第一帧会被
// 丢在还没有落点的时候 —— 那一版的表现就是永久黑屏。核心因此把参数集缓存下来、
// 给之后每个关键帧补齐（见 protocols/airplay/src/MirrorNalu.cpp）。
// 这里只管「见到参数集就重建格式描述」，不必自己去记。

import AVFoundation
import AppKit
import SwiftUI

/// 镜像帧的落点。由渲染面实现，核心每来一帧调一次。
protocol MirrorFrameSink: AnyObject {
    /// data 只在本次调用期间有效 —— 需要留存必须自己拷走。
    /// ptsUs 是发送端报的显示时间戳（微秒），0 表示它没给。
    func enqueueMirrorFrame(_ data: UnsafePointer<UInt8>, count: Int, isH265: Bool, ptsUs: Int64)
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
    // 没有落点的时候，帧是**静默**丢掉的 —— 而这是黑屏最常见的成因之一
    // （渲染面比第一帧晚一步挂上来）。所以「没落点」和「刚挂上」都要有记录。
    private var hasSink = false

    // 渲染面自己看不到日志窗口，而黑屏这类故障恰恰全都发生在渲染面内部。
    // 没有这条通道，「帧一直在计数、屏幕始终全黑」就只能靠猜。
    // 由 EngineModel 在启动时接上。
    private var logHandler: ((String) -> Void)?

    private init() {}

    func setLogHandler(_ handler: ((String) -> Void)?) {
        lock.lock()
        logHandler = handler
        lock.unlock()
    }

    func log(_ text: String) {
        lock.lock()
        let handler = logHandler
        lock.unlock()
        if let handler = handler {
            handler(text)
        }
    }

    func attach(_ sink: MirrorFrameSink?) {
        lock.lock()
        self.sink = sink
        let becameAttached = (sink != nil) && !hasSink
        hasSink = (sink != nil)
        lock.unlock()

        if becameAttached {
            log("镜像渲染面已挂上，开始接收帧。")
        }
    }

    func deliver(_ data: UnsafePointer<UInt8>, count: Int, isH265: Bool, ptsUs: Int64) {
        lock.lock()
        let target = sink
        lock.unlock()
        if let target = target {
            target.enqueueMirrorFrame(data, count: count, isH265: isH265, ptsUs: ptsUs)
        }
    }
}

/// 真正干活的视图：背层就是显示层。
final class MirrorRenderView: NSView, MirrorFrameSink {

    private let displayLayer = AVSampleBufferDisplayLayer()
    private var formatDescription: CMVideoFormatDescription?
    private var lastPresentationTime = CMTime.invalid
    // 第一帧的发送端时间戳，用来把整条时间轴重排到 0 起点。-1 表示还没收到。
    private var firstSenderPtsUs: Int64 = -1

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

    func enqueueMirrorFrame(_ data: UnsafePointer<UInt8>, count: Int, isH265: Bool, ptsUs: Int64) {
        guard count > 4 else {
            return
        }

        let nalUnits = MirrorRenderView.splitAnnexB(data, count: count)
        if nalUnits.isEmpty {
            noteDrop("这一帧里切不出 NALU")
            return
        }

        // 参数集可以夹在任何一帧里（切分辨率、切编码器、以及核心给关键帧补齐时），
        // 所以每帧都扫一遍，见到就重建格式描述。
        if let sets = MirrorRenderView.parameterSets(in: nalUnits, isH265: isH265) {
            rebuildFormatDescription(with: sets, isH265: isH265)
        }
        guard let format = formatDescription else {
            // 走到这里几乎只有一种可能：核心还没送来带参数集的帧。
            noteDrop("还没有解码参数（SPS/PPS）")
            return
        }

        // 解码器卡住时（比如刚切过分辨率）要主动冲一次，否则它会一直拒绝新帧。
        if displayLayer.requiresFlushToResumeDecoding {
            displayLayer.flush()
        }
        guard displayLayer.isReadyForMoreMediaData else {
            noteDrop("显示层未就绪")
            return   // 来不及就丢帧：镜像宁可按最新画面走，也不要越积越久
        }

        let avcc = MirrorRenderView.annexBToAVCC(nalUnits)
        guard !avcc.isEmpty else {
            noteDrop("转成 AVCC 之后是空的")
            return
        }
        let isKeyframe = MirrorRenderView.containsKeyframe(nalUnits, isH265: isH265)
        guard let sample = makeSampleBuffer(avcc: avcc, format: format,
                                            ptsUs: ptsUs, isKeyframe: isKeyframe) else {
            noteDrop("构造样例缓冲失败")
            return
        }
        displayLayer.enqueue(sample)
        noteEnqueued()
    }

    // MARK: - 诊断

    // 渲染这条路上每一步都是静默失败，而它们的表现完全一样：黑屏。
    // 所以每处丢弃都要写明原因，并做限流 —— 每帧一行会把日志淹掉。
    private var enqueuedFrames = 0
    private var droppedFrames = 0
    private var lastSummaryMs: Int64 = 0
    private var lastDropReason: String?
    private var loggedFormatFailure = false

    private func nowMs() -> Int64 {
        return Int64(Date().timeIntervalSince1970 * 1000)
    }

    private func noteEnqueued() {
        enqueuedFrames += 1
        if enqueuedFrames == 1 {
            MirrorFrameRouter.shared.log("镜像渲染：首帧已交给显示层。")
        }
        noteSummaryIfDue()
    }

    private func noteDrop(_ reason: String) {
        droppedFrames += 1
        lastDropReason = reason
        // 第一次丢弃一定报出来：黑屏的时候，这一行就是答案。
        if droppedFrames == 1 {
            MirrorFrameRouter.shared.log("镜像渲染：丢弃首帧 —— " + reason + "（已入队 "
                                         + String(enqueuedFrames) + " 帧）。")
        }
        noteSummaryIfDue()
    }

    private func noteSummaryIfDue() {
        // 第一次事件已经单独报过了，不再重复一句汇总。
        if enqueuedFrames + droppedFrames <= 1 {
            return
        }
        let now = nowMs()
        if now - lastSummaryMs < 5000 {
            return
        }
        lastSummaryMs = now

        var text = "镜像渲染：已入队 " + String(enqueuedFrames) + " 帧，丢弃 "
                   + String(droppedFrames) + " 帧"
        if droppedFrames > 0, let reason = lastDropReason {
            text += "（最近一次原因：" + reason + "）"
        }
        MirrorFrameRouter.shared.log(text + "。")
    }

    private func rebuildFormatDescription(with sets: [[UInt8]], isH265: Bool) {
        // 所有参数集拼进同一块连续存储。withUnsafeBufferPointer 给出的指针只在
        // 闭包内有效，把逐段取到的指针存进数组、出了闭包再拿来用是未定义行为
        // —— 前几版就是这么写的，能不能跑全看那块内存有没有被复用。
        var flat: [UInt8] = []
        var offsets: [Int] = []
        for set in sets {
            offsets.append(flat.count)
            flat.append(contentsOf: set)
        }
        if offsets.isEmpty || flat.isEmpty {
            return
        }

        var format: CMVideoFormatDescription?
        let status: OSStatus = flat.withUnsafeBufferPointer { buffer -> OSStatus in
            guard let base = buffer.baseAddress else { return -1 }
            var pointers: [UnsafePointer<UInt8>] = []
            for index in 0..<sets.count {
                pointers.append(base + offsets[index])
            }
            let sizes = sets.map { $0.count }
            return pointers.withUnsafeBufferPointer { pointerBuffer in
                sizes.withUnsafeBufferPointer { sizeBuffer in
                    if isH265 {
                        return CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                            allocator: kCFAllocatorDefault,
                            parameterSetCount: sets.count,
                            parameterSetPointers: pointerBuffer.baseAddress!,
                            parameterSetSizes: sizeBuffer.baseAddress!,
                            nalUnitHeaderLength: 4,
                            extensions: nil,
                            formatDescriptionOut: &format)
                    }
                    return CMVideoFormatDescriptionCreateFromH264ParameterSets(
                        allocator: kCFAllocatorDefault,
                        parameterSetCount: sets.count,
                        parameterSetPointers: pointerBuffer.baseAddress!,
                        parameterSetSizes: sizeBuffer.baseAddress!,
                        nalUnitHeaderLength: 4,
                        formatDescriptionOut: &format)
                }
            }
        }

        if status != noErr || format == nil {
            // 参数集拿到了却建不出解码配置，这是真正的异常，必须报出来。
            if !loggedFormatFailure {
                loggedFormatFailure = true
                MirrorFrameRouter.shared.log("镜像渲染：用收到的 SPS/PPS 建解码配置失败（错误码 "
                                             + String(status) + "），画面起不来。")
            }
            return
        }
        if let created = format {
            if formatDescription == nil {
                let dims = CMVideoFormatDescriptionGetDimensions(created)
                let kind = isH265 ? "H.265" : "H.264"
                MirrorFrameRouter.shared.log("镜像渲染：解码参数已就绪（" + kind + "），编码尺寸 "
                                             + String(dims.width) + "x" + String(dims.height) + "。")
            }
            formatDescription = created
        }
    }

    private func makeSampleBuffer(avcc: [UInt8], format: CMVideoFormatDescription,
                                  ptsUs: Int64, isKeyframe: Bool) -> CMSampleBuffer? {
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

        // 时间戳的取法见 nextPresentationTime：优先用发送端报的，那才是真实帧节奏。
        let pts = nextPresentationTime(senderUs: ptsUs)
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

        // NotSync 的意思就是字面那样：「这一帧不是随机访问点」。这里原先一律填
        // false，等于告诉解码器每一帧都能独立解码 —— 非关键帧会去参考根本不存在的
        // 参考帧，画面自然出不来。
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true),
           CFArrayGetCount(attachments) > 0 {
            let dict = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            // kCFBooleanTrue / kCFBooleanFalse 在 Swift 里被导入成可选值，这里显式解包
            // 而不是强解 —— 它们当然是恒非空的，但项目里不用 `!`。
            let notSync = isKeyframe ? kCFBooleanFalse : kCFBooleanTrue
            if let notSync = notSync {
                CFDictionarySetValue(dict,
                                     Unmanaged.passUnretained(kCMSampleAttachmentKey_NotSync).toOpaque(),
                                     Unmanaged.passUnretained(notSync).toOpaque())
            }
        }
        return sample
    }

    /// 下一个显示时间戳。
    ///
    /// 优先用发送端报的那个 —— 它才是真实的帧节奏（这台上次投的是 30fps），
    /// 而我们自己按 60fps 数出来的间隔只有实际的一半，画面会被放快一倍。
    ///
    /// 发送端的时钟是「开机以来的微秒」，是个很大的数，所以以第一帧为基准重排到
    /// 0 起点；时间戳又必须严格递增（同一时刻两帧会被显示层丢掉），所以相等或倒退
    /// 时往前推一毫秒。
    private func nextPresentationTime(senderUs: Int64) -> CMTime {
        let fallbackStep = CMTime(value: 1000, timescale: 1_000_000)   // 兜底按 1000fps 推进
        var candidate: CMTime
        if senderUs > 0 {
            if firstSenderPtsUs < 0 {
                firstSenderPtsUs = senderUs
            }
            let rebased = senderUs - firstSenderPtsUs
            candidate = CMTime(value: rebased, timescale: 1_000_000)
        } else if lastPresentationTime.isValid {
            candidate = lastPresentationTime + fallbackStep
        } else {
            candidate = CMTime.zero
        }
        if lastPresentationTime.isValid && candidate <= lastPresentationTime {
            candidate = lastPresentationTime + CMTime(value: 1, timescale: 1000)
        }
        lastPresentationTime = candidate
        return candidate
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

    /// 帧内是否含关键帧。H.264 的 IDR 是 5；H.265 的 IDR 是 19/20。
    /// 它决定 NotSync 怎么标，也决定这一帧能不能独立解出来。
    static func containsKeyframe(_ units: [[UInt8]], isH265: Bool) -> Bool {
        for unit in units where !unit.isEmpty {
            let type = isH265 ? (Int(unit[0]) >> 1) & 0x3F : Int(unit[0]) & 0x1F
            if isH265 {
                if type == 19 || type == 20 {
                    return true
                }
            } else if type == 5 {
                return true
            }
        }
        return false
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
