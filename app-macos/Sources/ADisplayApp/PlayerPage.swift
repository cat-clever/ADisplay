// ADisplay —— macOS 投屏播放页
//
// DLNA 给核心的只是一条 URL，解码与渲染交给系统播放器：AVPlayer 负责拉流、
// 硬件解码、音视频同步、HLS，AVPlayerView 连播放/暂停、进度拖动、音量、
// 全屏都一起给了。这一层只做三件事 —— 起播、执行手机发来的控制意图、
// 把播放器的真实状态回报给核心。
//
// 第三条是必须的，不是可选项：核心不碰播放器，手机的 GetTransportInfo /
// GetPositionInfo / GetMediaInfo / GetVolume 全靠这份回报作答。不回报的话
// 手机看到的永远是「正在起播」，进度条不动，音量滑块还会弹回去。

import AVKit
import SwiftUI

/// AVPlayerView 是 AppKit 控件，需要包一层。
///
/// 刻意用它而不是自绘控制条：传输控制（播放/暂停、进度、音量、全屏）都是
/// 现成的，行为和用户平时在系统里见到的一致，也省掉一大堆状态同步的坑。
private struct PlayerSurface: NSViewRepresentable {
    let player: AVPlayer

    func makeNSView(context: Context) -> AVPlayerView {
        let view = AVPlayerView()
        view.player = player
        view.controlsStyle = .floating
        view.allowsPictureInPicturePlayback = true
        return view
    }

    func updateNSView(_ view: AVPlayerView, context: Context) {
        if view.player !== player {
            view.player = player
        }
    }
}

@MainActor
final class PlayerViewModel: ObservableObject {

    let player = AVPlayer()

    private weak var engine: EngineModel?
    private var sessionId: UInt32 = 0

    private var timeObserver: Any?
    private var volumeObserver: NSKeyValueObservation?

    // 上一次回报过的状态与时长。相同就不重复回报 —— 位置变化很频繁，
    // 但状态和时长只在真变了时才值得回报，否则核心会推一串无意义的事件。
    private var lastState: PlaybackState?
    private var lastDurationMs: Int64 = -1

    deinit {
        if let observer = timeObserver {
            player.removeTimeObserver(observer)
        }
    }

    func attach(engine: EngineModel, media: EngineModel.ActiveMedia) {
        detach()

        self.engine = engine
        self.sessionId = media.sessionId

        guard let url = URL(string: media.url) else {
            engine.log("投屏地址无法解析，播放器起不来：\(media.url)", level: .error)
            return
        }

        engine.setPlaybackHandler { [weak self] command, value in
            self?.handle(command, value: value)
        }

        engine.log("开始拉流：\(media.url)")
        player.replaceCurrentItem(with: AVPlayerItem(url: url))
        player.volume = 1.0
        player.play()

        // 每秒回报一次位置。手机自己在轮询 GetPositionInfo，位置必须持续更新，
        // 否则手机上的进度条一直停在起点。
        let interval = CMTime(seconds: 1, preferredTimescale: 600)
        timeObserver = player.addPeriodicTimeObserver(forInterval: interval, queue: .main) {
            [weak self] _ in
            // 这里刻意用 Task 而不是 MainActor.assumeIsolated ——
            // 后者要 macOS 14 起才有，而本项目最低支持 macOS 12。
            Task { @MainActor in
                self?.report()
            }
        }

        // 音量也可能被用户直接拖控制条改掉，改了要回报给手机。
        volumeObserver = player.observe(\.volume, options: [.initial, .new]) { [weak self] player, _ in
            let percent = Int32((player.volume * 100).rounded())
            Task { @MainActor in
                self?.report(volumeOverride: percent)
            }
        }

        report()
    }

    func detach() {
        if let observer = timeObserver {
            player.removeTimeObserver(observer)
            timeObserver = nil
        }
        volumeObserver = nil
        engine?.setPlaybackHandler(nil)
        player.pause()
        player.replaceCurrentItem(with: nil)
        lastState = nil
        lastDurationMs = -1
    }

    // MARK: - 执行手机发来的意图

    private func handle(_ command: PlaybackCommand, value: Int64) {
        switch command {
        case .play:
            player.play()
        case .pause:
            player.pause()
        case .seek:
            // 容忍度给 0：DLNA 的拖动是「跳到这个位置」，不是「尽量接近」。
            player.seek(to: CMTime(value: value, timescale: 1000),
                        toleranceBefore: .zero, toleranceAfter: .zero)
        case .setVolume:
            let clamped = min(max(value, 0), 100)
            player.volume = Float(clamped) / 100.0
        case .setMute:
            player.isMuted = (value != 0)
        case .stop:
            // 「停止」由 EngineModel 处理：它会直接结束这次投屏，本页随之消失。
            break
        }

        // 控制条上的状态要立刻反映出来，不能等下一拍上报。
        report()
    }

    // MARK: - 回报状态

    private func report(volumeOverride: Int32? = nil) {
        guard let engine = engine, sessionId != 0 else { return }

        let state = currentState()
        let durationMs = durationMilliseconds()

        engine.reportPlayback(
            sessionId: sessionId,
            state: state,
            positionMs: positionMilliseconds(),
            // 位置每次都报（核心只用它答 GetPositionInfo）；
            // 状态与时长只在变了时才报，避免手机端被无意义的事件刷屏。
            durationMs: durationMs == lastDurationMs ? -1 : durationMs,
            volume: volumeOverride ?? -1,
            muted: -1
        )

        lastState = state
        if durationMs >= 0 {
            lastDurationMs = durationMs
        }
    }

    private func currentState() -> PlaybackState {
        if player.currentItem == nil {
            return .noMedia
        }
        if player.timeControlStatus == .waitingToPlayAtSpecifiedRate {
            return .transitioning
        }
        return player.rate > 0 ? .playing : .paused
    }

    private func positionMilliseconds() -> Int64 {
        let seconds = player.currentTime().seconds
        guard seconds.isFinite, seconds >= 0 else { return -1 }
        return Int64(seconds * 1000)
    }

    /// 直播流的 duration 是 NaN 或无穷，这种要按「未知」上报（-1），
    /// 不能直接转 Int64 —— 那会得到一个荒谬的负数，手机上的进度条会乱跳。
    private func durationMilliseconds() -> Int64 {
        guard let item = player.currentItem else { return -1 }
        let seconds = item.duration.seconds
        guard seconds.isFinite, seconds > 0 else { return -1 }
        return Int64(seconds * 1000)
    }
}

struct PlayerPage: View {

    @EnvironmentObject private var model: EngineModel
    @StateObject private var viewModel = PlayerViewModel()

    let media: EngineModel.ActiveMedia

    var body: some View {
        VStack(spacing: 12) {
            PlayerSurface(player: viewModel.player)
                .frame(minWidth: 560, minHeight: 315)

            HStack {
                Text("正在接收投屏")
                    .font(.headline)
                Spacer()
                Button("停止接收") {
                    model.stopCasting()
                }
            }
        }
        .padding(16)
        .frame(minWidth: 640, minHeight: 420)
        .onAppear {
            viewModel.attach(engine: model, media: media)
        }
        // 同一次投屏里手机换视频时 activeMedia 会变，要重新拉流。
        .onChange(of: media) { updated in
            viewModel.attach(engine: model, media: updated)
        }
        .onDisappear {
            viewModel.detach()
        }
    }
}
