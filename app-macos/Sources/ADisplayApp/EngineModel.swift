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

/// 设备名称不合规，message 可以直接显示给用户。
struct DeviceNameError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

@MainActor
final class EngineModel: ObservableObject {

    @Published private(set) var state: ServiceState = .stopped
    @Published private(set) var statusText: String = "未启动"
    @Published private(set) var logs: [String] = []
    @Published private(set) var localAddresses: String = ""
    @Published private(set) var deviceId: String = ""
    @Published private(set) var version: String = ""
    @Published private(set) var deviceName: String = ""

    /// 设备名称的编辑值。改这个不会立刻生效，要调 applyDeviceName()。
    @Published var deviceNameDraft: String = ""

    private var engine: OpaquePointer?
    private let maxLogLines = 500

    // 回调闭包必须由本对象持有并保活。
    private var stateCallback: (@convention(c) (UnsafeMutableRawPointer?, Int32) -> Void)?
    private var logCallback: (@convention(c) (UnsafeMutableRawPointer?, Int32, UnsafePointer<CChar>?) -> Void)?

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

        var callbacks = AdCallbacks()
        callbacks.struct_size = UInt32(MemoryLayout<AdCallbacks>.size)
        callbacks.on_state_changed = stateCallback
        callbacks.on_log = logCallback

        guard let handle = engine else { return }
        let installed = ad_engine_set_callbacks(handle, &callbacks, userData)
        if installed.rawValue != ResultCode.ok {
            appendLog(level: .error, text: "注册回调失败：\(describe(result: installed))")
        }
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
        }
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

    private func appendLog(level: LogLevel, text: String) {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        logs.append("[\(formatter.string(from: Date()))] [\(level.label)] \(text)")

        if logs.count > maxLogLines {
            logs.removeFirst(logs.count - maxLogLines)
        }
    }
}
