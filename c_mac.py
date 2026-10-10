# -*- coding: utf-8 -*-
import io, sys

def patch(path, pairs):
    with io.open(path, encoding='utf-8') as f:
        text = f.read()
    for old, new in pairs:
        n = text.count(old)
        if n != 1:
            sys.stderr.write('%s: anchor count=%d:\n----\n%s\n' % (path, n, old))
            sys.exit(1)
        text = text.replace(old, new)
    io.open(path, 'w', encoding='utf-8').write(text)
    print('ok ' + path)

EM = 'app-macos/Sources/ADisplayApp/EngineModel.swift'

patch(EM, [
(
"""private enum ResultCode {
    static let ok: UInt32 = 0
    static let bufferTooSmall: UInt32 = 9
}""",
"""private enum ResultCode {
    static let ok: UInt32 = 0
    static let bufferTooSmall: UInt32 = 9
    static let notFound: UInt32 = 10
}"""),
(
"""    /// 与 adisplay.h 的 AdQualityPreset 数值一一对应。""",
"""    /// 用户结束掉的那一次投屏：会话号与（DLNA 那条的）媒体地址。
    ///
    /// 会话本身没有结束 —— 核心那边还在，手机也还在推流 —— 只是我们不看它了。
    /// 待机页据此显示「继续观看 / 断开投屏」。
    private var dismissedSessionId: UInt32?
    private var dismissedMedia: ActiveMedia?

    /// 与 adisplay.h 的 AdQualityPreset 数值一一对应。"""),
(
"""    func stopCasting() {
        activeMedia = nil
        playbackHandler = nil""",
"""    func stopCasting() {
        // 先把「还能回去的那一次」记下来：下面马上要把这两处状态清掉了。
        // 镜像那条只有 mirrorSessionId，DLNA 那条只有 activeMedia。
        dismissedSessionId = mirrorSessionId ?? activeMedia?.sessionId
        dismissedMedia = activeMedia

        activeMedia = nil
        playbackHandler = nil"""),
(
"""    func resumeCasting() {
        guard castingDismissed else { return }
        MirrorAudioPlayer.shared.resume()
        castingDismissed = false
    }""",
"""    /// 点「继续观看」：回到投屏页。会话一直在，不需要重开会话。
    func resumeCasting() {
        guard castingDismissed else { return }
        MirrorAudioPlayer.shared.resume()
        castingDismissed = false

        if let media = dismissedMedia {
            // DLNA 那条：把媒体地址放回去，播放页自己会重新起播。
            dismissedMedia = nil
            dismissedSessionId = nil
            activeMedia = media
        } else if let id = dismissedSessionId {
            // 镜像那条：先清掉暂存再让渲染面挂上来。
            //
            // 暂存里缓的是「摘下时那一段」的头部若干帧，它们的发送端时间戳是旧的，
            // 回放会把时间轴基准定在过去、画面反而落后。所以清掉，等实时关键帧。
            dismissedSessionId = nil
            MirrorFrameRouter.shared.resetPending()
            mirrorSessionId = id
        }
    }

    /// 点「断开投屏」：让核心真的结束这一次会话。
    ///
    /// AirPlay 那条会让 iPhone 停下来；DLNA 只能本地结束并推一条 STOPPED 事件
    /// —— 接收端在 DLNA 里是被动方，协议上命令不了手机。
    func disconnectCasting() {
        guard let id = dismissedSessionId, let handle = engine else { return }

        let result = ad_engine_disconnect_session(handle, id)
        if result.rawValue == ResultCode.notFound {
            // 会话其实已经不在了（手机自己停的，而回调刚到）：自我修复，收掉横幅。
            dismissedSessionId = nil
            dismissedMedia = nil
            castingDismissed = false
            return
        }
        if result.rawValue != ResultCode.ok {
            appendLog(level: .warn, text: "断开投屏失败（\\\\(result.rawValue)）")
        }
        // 成功时不自作主张清状态：等核心的 session_closed 回调走 endMedia 收尾 ——
        // 那条路才是「会话真的没了」的权威。
    }"""),
])

CP = 'app-macos/Sources/ADisplayApp/ContentView.swift'
patch(CP, [
(
"""        VStack(alignment: .leading, spacing: 20) {
            deviceNameSection""",
"""        VStack(alignment: .leading, spacing: 20) {
            if model.castingDismissed {
                resumeBanner
            }
            deviceNameSection"""),
(
"""    /// 没有投屏时的设置页。""",
"""    /// 结束投屏之后，发送端往往还在推流 —— 给两个入口：回去看，或者真正断开。
    private var resumeBanner: some View {
        HStack(spacing: 12) {
            Text("投屏已结束，但手机仍在推流。")
                .foregroundStyle(.secondary)
            Spacer(minLength: 0)
            Button("继续观看") {
                model.resumeCasting()
            }
            Button("断开投屏") {
                model.disconnectCasting()
            }
        }
        .padding(12)
        .background(Color.gray.opacity(0.15), in: RoundedRectangle(cornerRadius: 10))
    }

    /// 没有投屏时的设置页。"""),
])
