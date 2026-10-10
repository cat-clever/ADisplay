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
import Combine
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
        // 画面按屏幕适应：整幅可见，多出来的边留黑。AVPlayerView 默认就是这个
        // 值，显式写出来免得将来被改成 resizeAspectFill —— 那个会把画面裁掉一块，
        // 而投屏看的就是完整画面。
        view.videoGravity = .resizeAspect
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

    /// 是不是在缓冲。拉流起不来、或者缓冲被掏空时都是 true。
    @Published private(set) var isBuffering = false

    /// 实测下载速度（字节/秒）。0 表示还不知道 —— 界面上就不显示数字。
    @Published private(set) var bytesPerSecond: Double = 0

    private var timeObserver: Any?
    private var volumeObserver: NSKeyValueObservation?
    private var bufferEmptyObserver: NSKeyValueObservation?
    private var keepUpObserver: NSKeyValueObservation?
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
                // 速度搭这趟车一起更新，不另起一个定时器。
                self.updateSpeed()
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

        // 缓冲状态看两路，缺一不可：
        //   isPlaybackBufferEmpty     缓冲被掏空了（正等着数据）
        //   isPlaybackLikelyToKeepUp  照这个速度接下来跟不跟得上
        // 只看前者，起播那一段会漏报（缓冲是空的但播放器还没开始等）；
        // 只看后者，卡顿中途会漏报。
        bufferEmptyObserver = player.currentItem?.observe(
            \.isPlaybackBufferEmpty, options: [.initial, .new]
        ) { [weak self] _, _ in
            guard let self = self else { return }
            Task { @MainActor in
                self.refreshBuffering()
            }
        }
        keepUpObserver = player.currentItem?.observe(
            \.isPlaybackLikelyToKeepUp, options: [.initial, .new]
        ) { [weak self] _, _ in
            guard let self = self else { return }
            Task { @MainActor in
                self.refreshBuffering()
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
        bufferEmptyObserver = nil
        keepUpObserver = nil
        isBuffering = false
        bytesPerSecond = 0
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

    /// 重新判断缓冲状态。两个 KVO 回调都走这里 —— 一个信号不足以判断。
    private func refreshBuffering() {
        guard let item = player.currentItem else {
            isBuffering = false
            return
        }
        isBuffering = item.isPlaybackBufferEmpty || !item.isPlaybackLikelyToKeepUp
    }

    /// 从访问日志里取实测码率。
    ///
    /// AVPlayer 自己记着这条 HTTP 连接跑了多少数据，比我们另外去数字节准，
    /// 也不用再挂一个定时器。日志要等传过一段数据才有，所以拿不到时保持 0，
    /// 界面就不显示数字（只显示「正在缓冲」）。
    private func updateSpeed() {
        guard let log = player.currentItem?.accessLog(), let last = log.events.last else {
            bytesPerSecond = 0
            return
        }
        // observedBitrate 的单位是比特/秒。
        bytesPerSecond = last.observedBitrate / 8.0
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

/// 鼠标移动侦测。
///
/// 桌面端没有遥控器，「鼠标在窗口里动了一下」就是用户想操作的信号，与
/// QuickTime、IINA 这些播放器的行为一致。
///
/// 用 NSTrackingArea 而不是 SwiftUI 的 onHover：后者只在进入/离开时各触发一次，
/// 鼠标停在窗口里不动的话，控件自动收起之后就再也叫不出来。
///
/// hitTest 返回 nil —— 这一层只报告鼠标位置，绝不参与命中判定。否则它会把点击
/// 从 AVPlayerView 手里抢走，播放器自带的播放/暂停、进度条、全屏按钮就全废了。
private struct MouseMoveReporter: NSViewRepresentable {
    let onMove: () -> Void

    func makeNSView(context: Context) -> NSView {
        let view = ReportingView()
        view.onMove = onMove
        return view
    }

    func updateNSView(_ nsView: NSView, context: Context) {
        if let view = nsView as? ReportingView {
            view.onMove = onMove
        }
    }

    private final class ReportingView: NSView {
        var onMove: (() -> Void)?

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            // 少了这一句，窗口根本不会投递 mouseMoved 事件。
            window?.acceptsMouseMovedEvents = true
        }

        override func updateTrackingAreas() {
            super.updateTrackingAreas()
            for area in trackingAreas {
                removeTrackingArea(area)
            }
            addTrackingArea(NSTrackingArea(
                rect: .zero,
                options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect],
                owner: self,
                userInfo: nil
            ))
        }

        override func mouseMoved(with event: NSEvent) {
            onMove?()
        }

        override func hitTest(_ point: NSPoint) -> NSView? {
            nil
        }
    }
}

struct PlayerPage: View {

    @EnvironmentObject private var model: EngineModel
    @StateObject private var viewModel = PlayerViewModel()

    /// 悬浮控件是否显示。默认收起 —— 投屏时用户看的就是画面。
    @State private var controlsVisible = false

    /// 自动收起的定时器。每次唤出都换一个新的，不攒着。
    @State private var hideWorkItem: DispatchWorkItem?

    /// 当前投屏的媒体地址。镜像会话没有地址，所以是可选的 ——
    /// 走哪条路由 mirrorSessionId 决定。
    let media: EngineModel.ActiveMedia?

    /// 控件出现后停留多久自动收起。
    private static let controlsTimeout: TimeInterval = 4

    var body: some View {
        // 画面铺满整个窗口，四周不留边距 —— 投屏时用户看的就是画面，那一圈
        // 留白是白扔的像素。控件**浮在画面上**，不占画面的高度：独占一行会把
        // 画面压扁一块，而那块地方本来该全是画面。
        ZStack {
            Group {
                if model.mirrorSessionId != nil {
                    // 镜像走这条：帧由核心直接交下来，渲染面自己解码显示，
                    // 不经过 AVPlayer —— 那是给「一条 URL」用的。
                    MirrorSurface()
                } else {
                    PlayerSurface(player: viewModel.player)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)

            // 只报鼠标移动，不抢点击（见 MouseMoveReporter）。
            //
            // frame 必须显式铺满：跟踪区域是按视图的可见矩形算的，不给尺寸就是
            // 零大小，mouseMoved 一次也不会来。hitTest 返回 nil，所以铺满也不会
            // 挡住下面的播放器。
            MouseMoveReporter { showControls() }
                .frame(maxWidth: .infinity, maxHeight: .infinity)

            // 缓冲提示。只在缓冲时出现 —— 全屏页上不该有常驻的东西。
            if viewModel.isBuffering {
                Text(bufferingLabel)
                    .font(.title3)
                    .padding(.horizontal, 22)
                    .padding(.vertical, 14)
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 10))
            }

            if controlsVisible {
                // 顶部状态条横贯整宽；按钮贴右侧竖排（与 Android / Windows 一致）。
                statusBar
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)

                controlBar
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .trailing)
            }
        }
        .frame(minWidth: 520, minHeight: 460)
        .onAppear {
            if let media = media {
                viewModel.attach(engine: model, media: media)
            }
            // 刚进投屏先把控件亮出来一次，让用户知道现在是什么状态、退路在哪。
            showControls()
        }
        // 同一次投屏里手机换视频时 activeMedia 会变，要重新拉流。
        // 镜像会话没有地址，这条不会触发。
        .onChange(of: media) { updated in
            if let updated = updated {
                viewModel.attach(engine: model, media: updated)
            }
        }
        .onDisappear {
            viewModel.detach()
            hideWorkItem?.cancel()
        }
    }

    /// 缓冲提示的文字。速度还不知道时就只说「正在缓冲」。
    private var bufferingLabel: String {
        if viewModel.bytesPerSecond >= 1024 * 1024 {
            return String(format: "正在缓冲　%.1f MB/s", viewModel.bytesPerSecond / 1024 / 1024)
        }
        if viewModel.bytesPerSecond >= 1024 {
            return String(format: "正在缓冲　%.0f KB/s", viewModel.bytesPerSecond / 1024)
        }
        return "正在缓冲…"
    }

    /// 顶部状态条。悬浮，不占画面高度。
    private var statusBar: some View {
        HStack(spacing: 12) {
            Text(model.deviceName + " · "
                 + (model.mirrorSessionId != nil ? "正在镜像屏幕" : "正在接收投屏"))
                .font(.headline)
            Spacer(minLength: 0)
            Text("鼠标移动可再显示控件")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        // 材质而不是纯黑：跟随浅色/深色外观，不用自己挑两套颜色。
        .background(.ultraThinMaterial)
    }

    /// 底部操作条。同样悬浮。
    private var controlBar: some View {
        // 竖排放在右侧：不排底部是因为播放器自带的进度条与传输控件就在窗口
        // 最下面（AVPlayerView 的浮层），两条挤在一起既看不清也点不准。
        VStack(spacing: 10) {
            Button("全屏") {
                toggleFullScreen()
            }
            Button("查看日志") {
                LogWindowController.shared.show()
            }
            // 「结束投屏」而不是「停止接收」：它只结束本地播放、回设置页，
            // 接收服务照旧跑着（见 EngineModel.stopCasting）—— 服务一停广播就
            // 撤了，手机那边立刻找不到这台机器。
            Button("结束投屏") {
                model.stopCasting()
            }
        }
        .padding(12)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 10))
        .padding(.trailing, 24)
    }

    private func showControls() {
        controlsVisible = true
        hideWorkItem?.cancel()

        // 每次唤出都重新计时：用户正在看控件，不该正好在这一刻收走。
        let work = DispatchWorkItem { controlsVisible = false }
        hideWorkItem = work
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.controlsTimeout, execute: work)
    }

    /// 窗口级全屏。与 Windows 端一致 —— 那边走的也是窗口全屏，
    /// 而不是只把画面填满窗口（那样标题栏还在，投屏时不是用户要的效果）。
    ///
    /// AVPlayerView 自带的那个全屏按钮做的事一样，所以两个入口不会打架。
    private func toggleFullScreen() {
        NSApplication.shared.keyWindow?.toggleFullScreen(nil)
    }
}
