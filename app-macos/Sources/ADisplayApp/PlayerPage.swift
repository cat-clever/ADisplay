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
import AppKit
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
        // 全屏按钮默认是关的，得显式打开 —— 投屏过来本来就是想在大屏上看，
        // 没有这个按钮等于每次都要手动拖窗口。
        view.showsFullScreenToggleButton = true
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
    private var endObserver: NSObjectProtocol?
    private var failureObserver: NSObjectProtocol?
    private var itemStatusObserver: NSKeyValueObservation?

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
            // 先 guard let 复制成 let 再进 Task。直接写 self?.report() 会报
            // "reference to captured var 'self' in concurrently-executing code" ——
            // [weak self] 捕出来的是个 var，而这两个闭包是 @Sendable 的。
            //
            // 用 Task 而不是 MainActor.assumeIsolated，是因为后者要 macOS 14
            // 起才有，而本项目最低支持 macOS 12。
            guard let self = self else { return }
            Task { @MainActor in
                self.report()
            }
        }

        // 音量也可能被用户直接拖控制条改掉，改了要回报给手机。
        volumeObserver = player.observe(\.volume, options: [.initial, .new]) { [weak self] player, _ in
            let percent = Int32((player.volume * 100).rounded())
            guard let self = self else { return }
            Task { @MainActor in
                self.report(volumeOverride: percent)
            }
        }

        // 拉流起不来的话，AVPlayer 既不播完也不触发 failedToPlayToEndTime ——
        // 它就那么挂着。只观察那两个通知的话，我们会一直回报 TRANSITIONING，
        // 手机那边看到的就是「投屏中但进度条不动」，而且永远不结束。
        // 所以必须盯 item.status。
        itemStatusObserver = player.currentItem?.observe(\.status, options: [.initial, .new]) {
            [weak self] item, _ in
            guard item.status == .failed else { return }
            let reason = item.error?.localizedDescription ?? "未知原因"
            guard let self = self else { return }
            Task { @MainActor in
                self.engine?.log("拉流失败：\(reason)", level: .error)
                self.report(forceState: .stopped)
            }
        }

        // 播完或者拉流失败都要回报一次，否则手机上会一直停在「播放中」。
        endObserver = NotificationCenter.default.addObserver(
            forName: AVPlayerItem.didPlayToEndTimeNotification,
            object: player.currentItem,
            queue: .main
        ) { [weak self] _ in
            guard let self = self else { return }
            Task { @MainActor in
                self.report(forceState: .stopped)
            }
        }

        failureObserver = NotificationCenter.default.addObserver(
            forName: AVPlayerItem.failedToPlayToEndTimeNotification,
            object: player.currentItem,
            queue: .main
        ) { [weak self] note in
            let reason = (note.userInfo?[AVPlayerItemFailedToPlayToEndTimeErrorKey] as? Error)?
                .localizedDescription ?? "未知原因"
            guard let self = self else { return }
            Task { @MainActor in
                self.engine?.log("拉流失败：\(reason)", level: .error)
                self.report(forceState: .stopped)
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
        itemStatusObserver = nil
        for observer in [endObserver, failureObserver] {
            if let observer = observer {
                NotificationCenter.default.removeObserver(observer)
            }
        }
        endObserver = nil
        failureObserver = nil
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
            // DLNA 的 Stop 是「停止播放」而不是「结束投屏」：媒体还挂着，
            // 手机随后可以再 Play。所以回到起点并暂停，不销毁会话。
            player.pause()
            player.seek(to: .zero, toleranceBefore: .zero, toleranceAfter: .zero)
        }

        // 控制条上的状态要立刻反映出来，不能等下一拍上报。
        report()
    }

    // MARK: - 回报状态

    private func report(volumeOverride: Int32? = nil, forceState: PlaybackState? = nil) {
        guard let engine = engine, sessionId != 0 else { return }

        let state = forceState ?? currentState()
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
        // 画面占满整个区域，四周不留边距 —— 投屏时用户看的就是画面，
        // 那一圈留白是白扔的像素。控制条紧贴在画面下方，所以画面本身仍然
        // 被内容包住，不需要额外的框。这一层与 Windows 端一致。
        VStack(spacing: 0) {
            PlayerSurface(player: viewModel.player)
                .frame(maxWidth: .infinity, maxHeight: .infinity)

            HStack(spacing: 12) {
                Text("正在接收投屏")
                    .font(.headline)
                Button("查看日志") {
                    LogWindowController.shared.show(model: model)
                }
                Button("全屏") {
                    toggleFullScreen()
                }
                Button("停止接收") {
                    model.stopCasting()
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 10)
        }
        .frame(minWidth: 520, minHeight: 460)
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

    /// 窗口级全屏。与 Windows 端一致 —— 那边走的也是窗口全屏，
    /// 而不是只把画面填满窗口（那样标题栏还在，投屏时不是用户要的效果）。
    ///
    /// AVPlayerView 自带的那个全屏按钮做的事一样，所以两个入口不会打架。
    private func toggleFullScreen() {
        NSApplication.shared.keyWindow?.toggleFullScreen(nil)
    }
}
