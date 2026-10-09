// ADisplay —— AirPlay 镜像伴音的输出
//
// 核心里已经把 AAC-ELD 解成了**交错 float32**（见 core/pipeline/AacEldDecoder），
// 这里只负责送进系统的音频单元。分工与视频那侧完全一致：核心解码、平台渲染。
//
// 不做重采样、也不做格式转换：核心给的就是 44100 / 立体声 / float32，
// 正好落在 AVAudioEngine 的原生格式里，多一层转换只会多一层延迟。

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

    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()
    private let lock = NSLock()

    private var format: AVAudioFormat?
    private var running = false
    private var queuedFrames = 0

    private init() {}

    /// 开始播放。重复调用是安全的。失败只记一行日志 —— 没有声音不该拖垮投屏。
    func start(sampleRate: Double, channels: Int) {
        lock.lock()
        let alreadyRunning = running
        lock.unlock()
        if alreadyRunning {
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
        queuedFrames = 0
        lock.unlock()

        MirrorFrameRouter.shared.log("镜像伴音：已开始播放（" + String(Int(sampleRate))
                                     + " Hz / " + String(channels) + " 声道）。")
    }

    func stop() {
        lock.lock()
        let wasRunning = running
        running = false
        format = nil
        queuedFrames = 0
        lock.unlock()

        if !wasRunning {
            return
        }
        player.stop()
        engine.stop()
        engine.detach(player)
    }

    /// 送一帧**交错** float32（LRLRLR…）。可以在任意线程调用。
    func enqueue(interleaved: UnsafePointer<Float>, frameCount: Int, channels: Int) {
        if frameCount <= 0 || channels <= 0 {
            return
        }

        lock.lock()
        if !running || format == nil {
            lock.unlock()
            return
        }
        if queuedFrames >= MirrorAudioPlayer.maxQueuedFrames {
            lock.unlock()   // 落后了：丢这一帧，跟住画面
            return
        }
        queuedFrames += 1
        let current = format
        lock.unlock()

        guard let current = current,
              let buffer = AVAudioPCMBuffer(pcmFormat: current,
                                            frameCapacity: AVAudioFrameCount(frameCount)) else {
            releaseSlot()
            return
        }
        buffer.frameLength = AVAudioFrameCount(frameCount)

        // AVAudioPCMBuffer 要的是**非交错**（每个声道一块），核心给的是交错 ——
        // 这里拆开。就这一处转换，没有别的。
        if let destination = buffer.floatChannelData {
            let channelCount = Int(current.channelCount)
            for channel in 0..<channelCount {
                let plane = destination[channel]
                for index in 0..<frameCount {
                    plane[index] = interleaved[index * channels + channel]
                }
            }
        }

        player.scheduleBuffer(buffer) { [weak self] in
            if let self = self {
                self.releaseSlot()
            }
        }
    }

    private func releaseSlot() {
        lock.lock()
        if queuedFrames > 0 {
            queuedFrames -= 1
        }
        lock.unlock()
    }
}
