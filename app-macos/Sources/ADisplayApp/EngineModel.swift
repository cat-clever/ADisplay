// ADisplay —— 核心库的 Swift 封装
//
// 与 C# 那边同样只做三件事：字符串编解码、错误转 Swift error、回调切主线程。
//
// 三个必须小心的地方：
//   1. C 回调是 @convention(c) 闭包，不能捕获上下文。所以用 userData 把 self
//      的裸指针带过去，回调里再还原成对象。
//   2. 回调在核心的工作线程上触发，碰 @Published 属性必须回到主线程，
//      否则 SwiftUI 会报「Publishing changes from background threads」。
//   3. C 枚举到 Swift 的导入方式随编译器版本有差异（成员名前缀可能被剥离，
//      rawValue 的宽度也不一定），所以这里一律用 rawValue 与整数字面量比较，
//      不写 AdResult.AD_OK 这种依赖导入细节的写法。

import Foundation
import CAdDisplay

/// 与 adisplay.h 的 AdServiceState 数值一一对应。
enum ServiceState: Int32 {
    case stopped = 0
    case starting = 1
    case running = 2
    case streaming = 3
    case stopping = 4
    case error = 5
}

/// 与 adisplay.h 的 AdResult 数值一一对应。
private enum ResultCode {
    static let ok: UInt32 = 0
    static let bufferTooSmall: UInt32 = 9
}

/// 与 adisplay.h 的 AdQualityPreset 数值一一对应。
enum QualityPreset: Int32 {
    case smooth = 0
    case balanced = 1
    case sharp = 2
}

/// 与 adisplay.h 的 AdLogLevel 数值一一对应。
enum LogLevel: Int32 {
    case trace = 0
    case debug = 1
    case info = 2
    case warn = 3
    case error = 4
    case off = 5

    var label: String {
        switch self {
        case .trace: return "TRACE"
        case .debug: return "DEBUG"
        case .info:  return "INFO"
        case .warn:  return "WARN"
        case .error: return "ERROR"
        case .off:   return "OFF"
        }
    }
}

/// 与 adisplay.h 的 AdTransportState 数值一一对应。
enum PlaybackState: Int32 {
    case noMedia = 0
    case stopped = 1
    case playing = 2
    case paused = 3
    case transitioning = 4
}

/// 与 adisplay.h 的 AdStreamKind 数值一一对应。
enum StreamKind: Int32 {
    case mirrorVideo = 0
    case mirrorAudio = 1
    case mediaURL = 2
}

/// 与 adisplay.h 的 AdPlaybackCommand 数值一一对应。
enum PlaybackCommand: Int32 {
    case play = 0
    case pause = 1
    case stop = 2
    case seek = 3        // value 是目标位置（毫秒）
    case setVolume = 4   // value 是 0..100
    case setMute = 5     // value 是 0 或 1
}

/// 设备名称不合规，message 可以直接显示给用户。
struct DeviceNameError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

@MainActor
final class EngineModel: ObservableObject {

    @Published private(set) var state: ServiceState = .stopped
    @Published private(set) var statusText: String = "未启动"
    @Published private(set) var localAddresses: String = ""
    @Published private(set) var deviceId: String = ""
    @Published private(set) var version: String = ""
    /// 画质档位。改动只影响手机下次连接 —— 尺寸是在应答 /info 时给出去的。
    @Published private(set) var qualityPreset: QualityPreset = .balanced
    @Published private(set) var deviceName: String = ""

    /// 设备名称的编辑值。改这个不会立刻生效，要调 applyDeviceName()。
    @Published var deviceNameDraft: String = ""

    /// 手机推来的媒体地址。非 nil 表示正在投屏，界面据此整窗切到播放页。
    @Published private(set) var activeMedia: ActiveMedia?

    /// 正在进行的 AirPlay 屏幕镜像会话。非 nil 表示画面由镜像流来，
    /// 界面要切到渲染面而不是播放器 —— 两者是同一页里的两条不同路径。
    @Published private(set) var mirrorSessionId: UInt32?

    /// 一次投屏会话。sessionId 要原样带回报给核心，核心靠它把状态
    /// 对应回手机上那个会话。
    struct ActiveMedia: Equatable {
        let sessionId: UInt32
        let url: String
    }

    /// 手机发来的播放控制意图，由播放页实现并执行。
    private var playbackHandler: ((PlaybackCommand, Int64) -> Void)?

    private var engine: OpaquePointer?

    // 回调闭包必须由本对象持有并保活。
    private var stateCallback: (@convention(c) (UnsafeMutableRawPointer?, Int32) -> Void)?
    private var logCallback: (@convention(c) (UnsafeMutableRawPointer?, Int32, UnsafePointer<CChar>?) -> Void)?
    private var mediaURLCallback: (@convention(c) (UnsafeMutableRawPointer?, UInt32, UnsafePointer<CChar>?, UnsafePointer<CChar>?) -> Void)?
    private var playbackCommandCallback: (@convention(c) (UnsafeMutableRawPointer?, UInt32, Int32, Int64) -> Void)?
    private var sessionClosedCallback: (@convention(c) (UnsafeMutableRawPointer?, UInt32, Int32) -> Void)?
    private var sessionOpenedCallback: (@convention(c) (UnsafeMutableRawPointer?, UInt32,
                                                        UnsafePointer<AdPeerInfo>?, Int32) -> Void)?
    private var mirrorFrameCallback: (@convention(c) (UnsafeMutableRawPointer?,
                                                      UnsafePointer<AdMirrorFrame>?) -> Void)?
    private var audioFrameCallback: (@convention(c) (UnsafeMutableRawPointer?,
                                                     UnsafePointer<AdAudioFrame>?) -> Void)?

    deinit {
        if let handle = engine {
            ad_engine_destroy(handle)
        }
    }

    // MARK: - 生命周期

    func create() {
        guard engine == nil else { return }

        var config = AdConfig()
        // 先取一份带默认值的配置（struct_size 与 abi_version 会由核心填好），
        // 再覆盖我们关心的字段。
        _ = ad_engine_get_default_config(&config)

        // 设备名、日志路径、配置路径留空：核心会取主机名作默认设备名，
        // 并把配置放到平台的惯例位置（macOS 上是 ~/Library/Application Support/ADisplay）。
        config.device_name = nil
        config.log_file_path = nil
        config.config_file_path = nil
        config.log_level = LogLevel.info.rawValue
        config.require_confirmation = 1

        var handle: OpaquePointer?
        let created = ad_engine_create(&config, &handle)

        guard created.rawValue == ResultCode.ok, let engineHandle = handle else {
            appendLog(level: .error, text: "创建引擎失败：\(describe(result: created))")
            return
        }
        engine = engineHandle

        installCallbacks()

        deviceName = readDeviceName()
        deviceNameDraft = deviceName
        deviceId = readDeviceId()
        localAddresses = readLocalAddresses()
        version = readVersion()
        qualityPreset = readQualityPreset()

        appendLog(level: .info, text: "ADisplay 已就绪，打开开关即可开始接收投屏。")
    }

    private func installCallbacks() {
        let userData = Unmanaged.passUnretained(self).toOpaque()

        stateCallback = { rawUserData, rawState in
            guard let rawUserData = rawUserData else { return }
            let model = Unmanaged<EngineModel>.fromOpaque(rawUserData).takeUnretainedValue()
            let newState = ServiceState(rawValue: rawState) ?? .error
            // 回调在工作线程上，改 @Published 前必须切回主线程。
            DispatchQueue.main.async {
                model.apply(state: newState)
            }
        }

        logCallback = { rawUserData, level, message in
            guard let rawUserData = rawUserData else { return }
            let model = Unmanaged<EngineModel>.fromOpaque(rawUserData).takeUnretainedValue()
            let text = message.map { String(cString: $0) } ?? ""
            let logLevel = LogLevel(rawValue: level) ?? .info
            DispatchQueue.main.async {
                model.appendLog(level: logLevel, text: text)
            }
        }

        mediaURLCallback = { rawUserData, sessionId, rawURL, _ in
            guard let rawUserData = rawUserData else { return }
            let model = Unmanaged<EngineModel>.fromOpaque(rawUserData).takeUnretainedValue()
            let url = rawURL.map { String(cString: $0) } ?? ""
            DispatchQueue.main.async {
                model.beginMedia(sessionId: sessionId, url: url)
            }
        }

        playbackCommandCallback = { rawUserData, sessionId, command, value in
            guard let rawUserData = rawUserData else { return }
            let model = Unmanaged<EngineModel>.fromOpaque(rawUserData).takeUnretainedValue()
            let parsed = PlaybackCommand(rawValue: command)
            DispatchQueue.main.async {
                guard let parsed = parsed else { return }
                model.dispatch(command: parsed, value: value, sessionId: sessionId)
            }
        }

        // 镜像会话与「媒体 URL」是两条不同的路：前者是持续的帧，后者是一条地址。
        // 这里只认前者，后者交给 on_media_url。
        sessionOpenedCallback = { rawUserData, sessionId, _, streamKind in
            guard let rawUserData = rawUserData else { return }
            guard streamKind == StreamKind.mirrorVideo.rawValue else { return }
            let model = Unmanaged<EngineModel>.fromOpaque(rawUserData).takeUnretainedValue()
            DispatchQueue.main.async {
                model.beginMirror(sessionId: sessionId)
            }
        }

        // 帧刻意不切回主线程：每帧跳一次会把解码队列压满、平白多出延迟。
        // 转接处自己加锁取指针，剩下的在调用线程上做完（显示层是线程安全的）。
        // 镜像伴音。与视频同理不切主线程 —— 音频帧更经不起排队，一跳主线程就
        // 可能晚几十毫秒，听感上是断续。核心里已经解成 PCM，这里直接交给播放端。
        audioFrameCallback = { _, frame in
            guard let frame = frame, let data = frame.pointee.data else { return }
            let channels = Int(frame.pointee.channels)
            let frames = Int(frame.pointee.frame_count)
            if channels <= 0 || frames <= 0 {
                return
            }
            let rate = Double(frame.pointee.sample_rate)
            if rate > 0 {
                MirrorAudioPlayer.shared.start(sampleRate: rate, channels: channels)
            }
            MirrorAudioPlayer.shared.enqueue(interleaved: data, frameCount: frames, channels: channels)
        }

        mirrorFrameCallback = { rawUserData, frame in
            guard rawUserData != nil, let frame = frame, let data = frame.pointee.data else { return }
            MirrorFrameRouter.shared.deliver(data,
                                             count: Int(frame.pointee.size),
                                             isH265: frame.pointee.is_h265 != 0,
                                             ptsUs: frame.pointee.pts_us)
        }

        sessionClosedCallback = { rawUserData, sessionId, _ in
            guard let rawUserData = rawUserData else { return }
            let model = Unmanaged<EngineModel>.fromOpaque(rawUserData).takeUnretainedValue()
            DispatchQueue.main.async {
                model.endMedia(sessionId: sessionId)
            }
        }

        var callbacks = AdCallbacks()
        callbacks.struct_size = UInt32(MemoryLayout<AdCallbacks>.size)
        callbacks.on_state_changed = stateCallback
        callbacks.on_log = logCallback
        callbacks.on_media_url = mediaURLCallback
        callbacks.on_playback_command = playbackCommandCallback
        callbacks.on_session_closed = sessionClosedCallback
        callbacks.on_session_opened = sessionOpenedCallback
        callbacks.on_mirror_frame = mirrorFrameCallback
        callbacks.on_audio_frame = audioFrameCallback

        // 渲染面的日志从这条路进日志窗口。黑屏那类故障全部发生在渲染面内部，
        // 那里没有别的通道能把「为什么这一帧没显示」说出来。
        MirrorFrameRouter.shared.setLogHandler { [weak self] text in
            DispatchQueue.main.async {
                if let self = self {
                    self.appendLog(level: .info, text: text)
                }
            }
        }

        guard let handle = engine else { return }
        let installed = ad_engine_set_callbacks(handle, &callbacks, userData)
        if installed.rawValue != ResultCode.ok {
            appendLog(level: .error, text: "注册回调失败：\(describe(result: installed))")
        }
    }

    // MARK: - 投屏播放（文档 3.2、4.1）
    //
    // 核心不播放：DLNA 给的是一条 URL，解码渲染交给系统播放器（见 PlayerPage）。
    // 这一节负责把两边接起来，并保存播放页的位置 —— 手机的 Get*Info 是同步
    // 应答，答案得立刻能给出。

    /// 由播放页注册。手机的控制意图会转到这里执行。
    func setPlaybackHandler(_ handler: ((PlaybackCommand, Int64) -> Void)?) {
        playbackHandler = handler
    }

    /// 用户点「结束投屏」。只结束本地播放、回设置页，**不停接收服务** ——
    /// 服务一停广播就撤了，手机那边立刻找不到这台机器。核心那边下次收到推送
    /// 会重新开会话。
    func stopCasting() {
        activeMedia = nil
        playbackHandler = nil
        // 镜像那条路没有播放器可停，停的就是「还往渲染面送帧」这件事。
        mirrorSessionId = nil
        MirrorFrameRouter.shared.attach(nil)
        // 上一个会话暂存的帧绝不能喂给下一个会话的解码器。
        MirrorFrameRouter.shared.resetPending()
    }

    /// 把播放器的真实状态回报给核心。不回报的话手机看到的永远停在「起播中」。
    ///
    /// volume / muted 传 -1 表示「这项没变」—— 用 -1 而不是 0，因为 0 是
    /// 合法值（音量 0、未静音），拿 0 当不变会把真实状态冲掉。
    func reportPlayback(sessionId: UInt32, state: PlaybackState, positionMs: Int64,
                        durationMs: Int64, volume: Int32, muted: Int32) {
        guard let handle = engine else { return }

        var status = AdPlaybackStatus()
        status.struct_size = UInt32(MemoryLayout<AdPlaybackStatus>.size)
        status.abi_version = ad_abi_version()
        status.session_id = sessionId
        status.transport_state = state.rawValue
        status.position_ms = positionMs
        status.duration_ms = durationMs
        status.volume = volume
        status.muted = muted

        _ = ad_engine_report_playback(handle, &status)
    }

    /// 核心日志之外的界面侧日志。用同一个缓冲，用户复制日志时能看到全貌。
    func log(_ text: String, level: LogLevel = .info) {
        appendLog(level: level, text: text)
    }

    private func beginMedia(sessionId: UInt32, url: String) {
        activeMedia = ActiveMedia(sessionId: sessionId, url: url)
        appendLog(level: .info, text: "手机推送媒体：\(url)")
    }

    private func beginMirror(sessionId: UInt32) {
        mirrorSessionId = sessionId
        appendLog(level: .info, text: "iPhone 开始屏幕镜像")
    }

    private func endMedia(sessionId: UInt32) {
        // 镜像会话与媒体会话共用同一个会话号空间，两边都要按会话号收尾。
        if mirrorSessionId == sessionId {
            mirrorSessionId = nil
            // 会话结束了就不该再往渲染面送帧，也不该继续放声音。
            MirrorFrameRouter.shared.attach(nil)
            MirrorAudioPlayer.shared.stop()
            // 暂存里可能是流开头的参数集与关键帧 —— 但它们属于刚结束的这个会话，
            // 下一个会话有自己的一套，混着喂解码器会出错。
            MirrorFrameRouter.shared.resetPending()
            appendLog(level: .info, text: "屏幕镜像已结束")
        }

        // 会话号对不上说明是上一个已经被抢占的会话在收尾，忽略即可。
        guard activeMedia?.sessionId == sessionId else { return }
        activeMedia = nil
        playbackHandler = nil
    }

    private func dispatch(command: PlaybackCommand, value: Int64, sessionId: UInt32) {
        guard let media = activeMedia, media.sessionId == sessionId else { return }

        // 「停止」不做特殊处理：DLNA 的 Stop 只是停止播放，媒体仍然挂着，
        // 手机随后可以再 Play。真正结束会话的是手机推来新地址（旧会话被抢占）
        // 或者接收服务停止，那两条走 session_closed。
        playbackHandler?(command, value)
    }

    // MARK: - 服务开关

    func start() {
        guard let handle = engine else { return }

        let result = ad_engine_start(handle)
        guard result.rawValue == ResultCode.ok else {
            let detail = readLastError() ?? describe(result: result)
            appendLog(level: .error, text: "启动失败：\(detail)")
            // 状态由核心通过回调置为 error，这里不重复设置，避免两处打架。
            return
        }
    }

    func stop() {
        guard let handle = engine else { return }
        ad_engine_stop(handle)
    }

    var isRunning: Bool {
        return state == .running || state == .streaming
    }

    func toggleService() {
        if isRunning {
            stop()
        } else {
            start()
        }
    }

    func setQualityPreset(_ preset: QualityPreset) {
        guard let handle = engine else { return }
        let result = ad_engine_set_quality_preset(handle, preset.rawValue)
        if result.rawValue != ResultCode.ok {
            appendLog(level: .error, text: "切换画质档位失败：\(describe(result: result))")
            return
        }
        qualityPreset = preset
    }

    /// 读回已保存的档位。界面启动时用它把选择器拨到正确的那一档 ——
    /// 不读的话每次启动都会显示成默认值，而实际用的是上次选的那个。
    private func readQualityPreset() -> QualityPreset {
        guard let handle = engine else { return .balanced }
        var raw: Int32 = QualityPreset.balanced.rawValue
        if ad_engine_get_quality_preset(handle, &raw).rawValue != ResultCode.ok {
            return .balanced
        }
        if let preset = QualityPreset(rawValue: raw) {
            return preset
        }
        return .balanced
    }

    // MARK: - 设备名称

    /// 校验草稿值。合规返回 nil，不合规返回可以直接显示给用户的原因。
    func validateDeviceNameDraft() -> String? {
        var reasonBuffer = [CChar](repeating: 0, count: 1024)

        let result = deviceNameDraft.withCString { pointer in
            ad_device_name_validate(pointer, &reasonBuffer, reasonBuffer.count)
        }

        if result.rawValue == ResultCode.ok {
            return nil
        }
        let text = String(cString: reasonBuffer)
        return text.isEmpty ? "设备名称不合规" : text
    }

    func applyDeviceName() throws {
        guard let handle = engine else { return }

        let result = deviceNameDraft.withCString { pointer in
            ad_engine_set_device_name(handle, pointer)
        }

        guard result.rawValue == ResultCode.ok else {
            throw DeviceNameError(message: validateDeviceNameDraft() ?? "设备名称不合规")
        }

        deviceName = readDeviceName()
        appendLog(level: .info, text: "设备名称已保存为「\(deviceName)」，手机端列表可能需要几秒刷新。")
    }

    // MARK: - 读取字符串
    //
    // C 侧约定：缓冲区不够时返回 BufferTooSmall 并把所需字节数写进 out_length。
    // 所以先用小缓冲探一次，再按需要的尺寸重来。

    private func readDeviceName() -> String {
        guard let handle = engine else { return "" }
        return readString { buffer, size, outLength in
            ad_engine_get_device_name(handle, buffer, size, outLength)
        }
    }

    private func readDeviceId() -> String {
        guard let handle = engine else { return "" }
        return readString { buffer, size, outLength in
            ad_engine_get_device_id(handle, buffer, size, outLength)
        }
    }

    private func readLocalAddresses() -> String {
        guard let handle = engine else { return "" }
        return readString { buffer, size, outLength in
            ad_engine_get_local_addresses(handle, buffer, size, outLength)
        }
    }

    private func readLastError() -> String? {
        guard let handle = engine else { return nil }
        let text = readString { buffer, size, outLength in
            ad_engine_get_last_error(handle, buffer, size, outLength)
        }
        return text.isEmpty ? nil : text
    }

    private func readVersion() -> String {
        let text = ad_version_string()
        guard let pointer = text else { return "未知" }
        return String(cString: pointer)
    }

    private typealias StringReader =
        (UnsafeMutablePointer<CChar>?, Int, UnsafeMutablePointer<Int>?) -> AdResult

    private func readString(_ reader: StringReader) -> String {
        var probe = [CChar](repeating: 0, count: 256)
        var needed = 0
        let first = reader(&probe, probe.count, &needed)

        if first.rawValue == ResultCode.ok {
            return String(cString: probe)
        }
        guard first.rawValue == ResultCode.bufferTooSmall, needed > 0, needed <= 1024 * 1024 else {
            return ""
        }

        var buffer = [CChar](repeating: 0, count: needed)
        var ignored = 0
        let second = reader(&buffer, buffer.count, &ignored)
        return second.rawValue == ResultCode.ok ? String(cString: buffer) : ""
    }

    private func describe(result: AdResult) -> String {
        let text = ad_result_string(result)
        guard let pointer = text else { return "错误码 \(result.rawValue)" }
        return String(cString: pointer)
    }

    // MARK: - 内部状态

    private func apply(state newState: ServiceState) {
        state = newState

        switch newState {
        case .stopped:   statusText = "未启动"
        case .starting:  statusText = "正在启动…"
        case .running:   statusText = "正在广播，等待手机连接"
        case .streaming: statusText = "正在投屏"
        case .stopping:  statusText = "正在停止…"
        case .error:     statusText = "启动失败"
        }
    }

    /// 日志写进独立的 LogStore，而不是本类的 @Published ——
    /// 投屏页与设置页都在观察本类，日志挂在这里会让它们每来一行就重排一次
    /// （代价随日志量放大，全屏切换时尤其明显）。理由详见 LogStore.swift。
    private func appendLog(level: LogLevel, text: String) {
        LogStore.shared.append(level: level, text: text)
    }
}
