// ADisplay —— 界面文字表（macOS 端）
//
// 与电视端同一套思路（见 app-tv 的 Strings.kt）：自己拿一份表，语言一变就换一份、
// 界面自然重画。系统那套（Localizable.strings + Bundle 的本地化）是按启动时的
// 语言在取一次值，改语言要么重启应用、要么手动指定 lproj —— 用户要求**切换后不重启**
// （2026-10-11），所以走自己这一份。
//
// 只放**界面上的字**。日志不在这里 —— 日志一律中文，那是给人排查用的（同一个决定）。
// 协议里给手机看的字符串（设备名、型号、mDNS 的 TXT）也不在这里：那是数据，不是界面文字。

import Foundation

/// 界面语言。只有两种 —— 需求就是中英双语。
enum Language: String {
    case zh
    case en

    /// 从系统语言推断默认值：**只有明确是英文才用英文，其余一律中文**。
    static func fromSystem() -> Language {
        guard let tag = Locale.preferredLanguages.first else {
            return .zh
        }
        return tag.lowercased().hasPrefix("en") ? .en : .zh
    }

    static func ofStored(_ value: String?) -> Language {
        guard let value = value, let language = Language(rawValue: value) else {
            return .zh
        }
        return language
    }
}

/// 一份语言对应的全部界面文字。
///
/// 两个实例常驻（[chineseStrings] / [englishStrings]），切换语言只换引用。
protocol Strings {
    var language: Language { get }

    // ---- 服务状态 ----
    var stateStopped: String { get }
    var stateStarting: String { get }
    var stateRunning: String { get }
    var stateStreaming: String { get }
    var stateStopping: String { get }
    var stateError: String { get }

    // ---- 结束投屏后的横幅 ----
    var castingEndedBanner: String { get }
    var resumeCasting: String { get }
    var disconnectCasting: String { get }

    // ---- 设备名称 ----
    var deviceNameLabel: String { get }
    var deviceNamePlaceholder: String { get }
    var deviceNameHint: String { get }
    var deviceNameInvalid: String { get }
    var save: String { get }

    // ---- 画质档位 ----
    var mirrorQuality: String { get }
    var qualitySmooth: String { get }
    var qualityBalanced: String { get }
    var qualitySharp: String { get }
    /// 三个分辨率从 QualityPreset 传进来（不在这里抄一份数）。
    func qualityHint(smooth: String, balanced: String, sharp: String) -> String

    // ---- 服务开关 ----
    var receiverService: String { get }
    var startReceiving: String { get }

    // ---- 本机信息 ----
    var localInfo: String { get }
    var infoLocalAddress: String { get }
    var infoDeviceId: String { get }
    var infoCoreVersion: String { get }
    var noLocalAddress: String { get }
    /// 信息行的「标签：值」中间那个冒号（中文用全角，英文用半角加空格）。
    var labelColon: String { get }
    /// 多个局域网地址之间的分隔。
    var addressSeparator: String { get }

    // ---- 日志 ----
    var logLabel: String { get }
    var viewLog: String { get }
    var logHint: String { get }
    var logWindowTitle: String { get }
    var copyAll: String { get }
    var clearLog: String { get }

    // ---- 界面语言 ----
    var uiLanguage: String { get }

    // ---- 播放 / 镜像页 ----
    var mirroringScreen: String { get }
    var receivingCast: String { get }
    var playbackHintMouse: String { get }
    var fullScreen: String { get }
    var endCasting: String { get }
    var buffering: String { get }
    /// 格式串（`%.1f` 这类由平台自己解释），下面两个是码率单位不同的两种写法。
    var bufferingMBFormat: String { get }
    var bufferingKBFormat: String { get }
}

extension Strings {
    /// 服务状态的文字。状态码是核心给的，文字在这里选 —— 枚举里存不了双语。
    func stateText(_ state: ServiceState) -> String {
        switch state {
        case .stopped:   return stateStopped
        case .starting:  return stateStarting
        case .running:   return stateRunning
        case .streaming: return stateStreaming
        case .stopping:  return stateStopping
        case .error:     return stateError
        }
    }

    /// 画质档位的名字（界面用）。日志里那份中文仍写在枚举上 —— 日志一律中文。
    func qualityLabel(_ preset: QualityPreset) -> String {
        switch preset {
        case .smooth:   return qualitySmooth
        case .balanced: return qualityBalanced
        case .sharp:    return qualitySharp
        }
    }
}

struct ChineseStrings: Strings {
    let language = Language.zh

    let stateStopped = "未启动"
    let stateStarting = "正在启动…"
    let stateRunning = "正在广播，等待手机连接"
    let stateStreaming = "正在投屏"
    let stateStopping = "正在停止…"
    let stateError = "启动失败"

    let castingEndedBanner = "投屏已结束，但手机仍在推流。"
    let resumeCasting = "继续观看"
    let disconnectCasting = "断开投屏"

    let deviceNameLabel = "设备名称"
    let deviceNamePlaceholder = "例如：客厅电脑"
    let deviceNameHint = "这是手机投屏列表里显示的名字，修改后立即生效，不需要重新配对。"
    let deviceNameInvalid = "设备名称不合规"
    let save = "保存"

    let mirrorQuality = "画质"
    let qualitySmooth = "流畅"
    let qualityBalanced = "均衡"
    let qualitySharp = "高清"

    func qualityHint(smooth: String, balanced: String, sharp: String) -> String {
        return "画质决定手机按多大的分辨率把屏幕编出来：流畅 " + smooth + "、均衡 " + balanced
            + "、高清 " + sharp + "。桌面全屏看推荐「高清」，低端盒子用「流畅」。改动在手机下次连上时生效。"
    }

    let receiverService = "接收服务"
    let startReceiving = "开启接收服务"

    let localInfo = "本机信息"
    let infoLocalAddress = "局域网地址"
    let infoDeviceId = "设备标识"
    let infoCoreVersion = "核心版本"
    let noLocalAddress = "（未检测到可用的局域网地址）"
    let labelColon = "："
    let addressSeparator = "，"

    let logLabel = "日志"
    let viewLog = "查看日志"
    let logHint = "日志在单独的窗口里显示，投屏时也能一直开着。"
    let logWindowTitle = "ADisplay 日志"
    let copyAll = "复制全部"
    let clearLog = "清除日志"

    let uiLanguage = "界面语言"

    let mirroringScreen = "正在镜像屏幕"
    let receivingCast = "正在接收投屏"
    let playbackHintMouse = "鼠标移动可再显示控件"
    let fullScreen = "全屏"
    let endCasting = "结束投屏"
    let buffering = "正在缓冲…"
    let bufferingMBFormat = "正在缓冲　%.1f MB/s"
    let bufferingKBFormat = "正在缓冲　%.0f KB/s"
}

struct EnglishStrings: Strings {
    let language = Language.en

    let stateStopped = "Stopped"
    let stateStarting = "Starting…"
    let stateRunning = "Broadcasting — waiting for a phone"
    let stateStreaming = "Casting"
    let stateStopping = "Stopping…"
    let stateError = "Failed to start"

    let castingEndedBanner = "Casting ended, but the phone is still streaming."
    let resumeCasting = "Resume"
    let disconnectCasting = "Disconnect"

    let deviceNameLabel = "Device name"
    let deviceNamePlaceholder = "e.g. Living Room Mac"
    let deviceNameHint = "This is the name shown in the phone's cast list. Changes take effect immediately; no re-pairing needed."
    let deviceNameInvalid = "That device name isn't allowed"
    let save = "Save"

    let mirrorQuality = "Quality"
    let qualitySmooth = "Smooth"
    let qualityBalanced = "Balanced"
    let qualitySharp = "Sharp"

    func qualityHint(smooth: String, balanced: String, sharp: String) -> String {
        return "Sets how large a screen the phone encodes: Smooth " + smooth + ", Balanced "
            + balanced + ", Sharp " + sharp + ". Use Sharp for full-screen on a desktop display, "
            + "Smooth on a low-end TV box. Takes effect the next time the phone connects."
    }

    let receiverService = "Receiver"
    let startReceiving = "Start receiving"

    let localInfo = "This machine"
    let infoLocalAddress = "Local address"
    let infoDeviceId = "Device ID"
    let infoCoreVersion = "Core version"
    let noLocalAddress = "(no usable local address detected)"
    let labelColon = ": "
    let addressSeparator = ", "

    let logLabel = "Log"
    let viewLog = "View log"
    let logHint = "The log lives in its own window, so it can stay open while casting."
    let logWindowTitle = "ADisplay Log"
    let copyAll = "Copy all"
    let clearLog = "Clear log"

    let uiLanguage = "Language"

    let mirroringScreen = "Mirroring screen"
    let receivingCast = "Receiving cast"
    let playbackHintMouse = "Move the mouse to show the controls"
    let fullScreen = "Full screen"
    let endCasting = "End casting"
    let buffering = "Buffering…"
    let bufferingMBFormat = "Buffering %.1f MB/s"
    let bufferingKBFormat = "Buffering %.0f KB/s"
}

let chineseStrings: Strings = ChineseStrings()
let englishStrings: Strings = EnglishStrings()

/// 界面语言存哪、怎么读。存 UserDefaults：这是纯界面层的偏好，不该去动 C ABI。
enum LanguageStore {

    private static let key = "ADisplayLanguage"

    /// 存下来的语言；没存过就按系统语言定（非英文一律中文）。
    static func read() -> Language {
        if let stored = UserDefaults.standard.string(forKey: key) {
            return Language.ofStored(stored)
        }
        return Language.fromSystem()
    }

    static func write(_ language: Language) {
        UserDefaults.standard.set(language.rawValue, forKey: key)
    }

    static func strings() -> Strings {
        return read() == .en ? englishStrings : chineseStrings
    }
}
