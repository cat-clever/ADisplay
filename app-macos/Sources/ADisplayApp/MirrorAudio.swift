// ADisplay —— AirPlay 镜像伴音的输出
//
// 核心里已经把 AAC-ELD 解成了**交错 float32**（见 core/pipeline/AacEldDecoder），
// 这里只负责送进系统的音频单元。分工与视频那侧完全一致：核心解码、平台渲染。
//
// 一个要点：**攒成大块再送**。
//
// 核心交下来的是每帧 480 个样本、每秒约 92 帧 —— 直接一帧一次 scheduleBuffer
// 的话，等于每秒往引擎里塞 92 个小缓冲，而且它们的采样率（44100）通常与设备
// 不一致。AVAudioEngine 的转换器在「又小又不齐」的喂法下代价很高：实测这条路
// 让 CPU 多出十几个点，而核心侧计时显示我们自己的解码与拆包只占 1.5~3%，
// 多出来的都在引擎内部的渲染与重采样上。
//
// 所以这里攒到固定帧数再送：每秒约十次，每次大小一致，转换器不必反复重新对齐。

import AVFoundation

/// 镜像伴音的播放端。
///
/// 刻意不做成 @MainActor：帧从核心线程来，每帧跳一次主线程会把音频队列压满、
/// 平白多出一大截延迟。AVAudioEngine 本来就是线程安全的。
final class MirrorAudioPlayer {

    static let shared = MirrorAudioPlayer()

    // 已在播放器里排队、尚未播完的帧数上限。伴音与画面同源，越积越久只会让
    // 声音越来越滞后；宁可丢掉一小段，也要跟住画面。
    private static let maxQueuedFrames = 8

    // 攒够这么多帧才送一次。4096 帧 ≈ 93 毫秒，是延迟与效率之间常见的折中：
    // 再小就退回「又小又不齐」，再大则口型对不上。
    private static let chunkFrames = 4096
    // 队伍上限按**块**算：落后了就丢，跟住画面。
    private static let maxQueuedChunks = 4

    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()
    private let lock = NSLock()

    private var format: AVAudioFormat?
    private var running = false
    /// 挂起：停止播放，并且**不再因为收到新帧而自动重启**。
    ///
    /// 用户点「结束投屏」之后手机还在推流，帧会一直进来；而 start() 见
    /// running == false 就会重新 attach/connect/play —— 表现就是「画面关了，
    /// 声音还在」。这个标志就是那道门禁，要重新出声必须显式 resume()。
    private var suspended = false
    private var queuedChunks = 0

    // 尚未凑满一块的交错样本（LRLRLR…）。
    private var pending: [Float] = []
    private var pendingChannels = 0

    private init() {}

    /// 开始播放。重复调用是安全的。失败只记一行日志 —— 没有声音不该拖垮投屏。
    func start(sampleRate: Double, channels: Int) {
        lock.lock()
        let alreadyRunning = running
        let isSuspended = suspended
        lock.unlock()
        if alreadyRunning || isSuspended {
            return
        }

        guard let created = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                          sampleRate: sampleRate,
                                          channels: AVAudioChannelCount(channels),
                                          interleaved: false) else {
            MirrorFrameRouter.shared.log("镜像伴音：这个采样率/声道数建不出音频格式，这一路没有声音。")
            return
        }

        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: created)
        do {
            try engine.start()
        } catch {
            MirrorFrameRouter.shared.log("镜像伴音：音频引擎起不来（"
                                         + error.localizedDescription + "），这一路没有声音。")
            engine.detach(player)
            return
        }
        player.play()

        lock.lock()
        format = created
        running = true
        queuedChunks = 0
        pending.removeAll(keepingCapacity: true)
        pendingChannels = 0
        lock.unlock()

        MirrorFrameRouter.shared.log("镜像伴音：已开始播放（" + String(Int(sampleRate))
                                     + " Hz / " + String(channels) + " 声道）。")
    }

    func stop() {
        lock.lock()
        let wasRunning = running
        running = false
        // stop 的语义是「停止且不许自己重启」，要重新出声必须显式 resume。
        // 这一句就是「结束投屏之后声音还会回来」的解法。
        suspended = true
        format = nil
        queuedChunks = 0
        pending.removeAll(keepingCapacity: true)
        pendingChannels = 0
        lock.unlock()

        if !wasRunning {
            return
        }
        player.stop()
        engine.stop()
        engine.detach(player)
    }

    /// 解除挂起：下一次收到帧时重新把引擎建起来。用于「继续观看」与新一次投屏。
    func resume() {
        lock.lock()
        suspended = false
        lock.unlock()
    }

    /// 收一帧**交错** float32（LRLRLR…）。可以在任意线程调用。
    ///
    /// 攒够一块（见 chunkFrames）才真正送进引擎 —— 理由见文件开头。
    func enqueue(interleaved: UnsafePointer<Float>, frameCount: Int, channels: Int) {
        if frameCount <= 0 || channels <= 0 {
            return
        }

        lock.lock()
        if !running || format == nil || queuedChunks >= MirrorAudioPlayer.maxQueuedChunks {
            lock.unlock()   // 落后了：丢这一帧，跟住画面
            return
        }
        pendingChannels = channels
        pending.append(contentsOf: UnsafeBufferPointer(start: interleaved,
                                                       count: frameCount * channels))
        if pending.count < MirrorAudioPlayer.chunkFrames * channels {
            lock.unlock()
            return
        }

        let samples = pending
        pending.removeAll(keepingCapacity: true)
        queuedChunks += 1
        let current = format
        lock.unlock()

        guard let current = current else {
            releaseChunk()
            return
        }
        schedule(samples, channels: channels, format: current)
    }

    private func schedule(_ interleaved: [Float], channels: Int, format: AVAudioFormat) {
        let frameCount = interleaved.count / channels
        guard let buffer = AVAudioPCMBuffer(pcmFormat: format,
                                            frameCapacity: AVAudioFrameCount(frameCount)) else {
            releaseChunk()
            return
        }
        buffer.frameLength = AVAudioFrameCount(frameCount)

        // AVAudioPCMBuffer 要的是**非交错**（每声道一块），核心给的是交错 ——
        // 这里拆开。就这一处转换，没有别的。
        if let destination = buffer.floatChannelData {
            let channelCount = min(channels, Int(format.channelCount))
            for channel in 0..<channelCount {
                let plane = destination[channel]
                for index in 0..<frameCount {
                    plane[index] = interleaved[index * channels + channel]
                }
            }
        }

        player.scheduleBuffer(buffer) { [weak self] in
            if let self = self {
                self.releaseChunk()
            }
        }
    }

    private func releaseChunk() {
        lock.lock()
        if queuedChunks > 0 {
            queuedChunks -= 1
        }
        lock.unlock()
    }
}
