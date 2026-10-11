// ADisplay —— 界面文字表（电视端）
//
// 为什么不用 Android 的资源系统（values/ + values-en/ + strings.xml）：
// 用户要求**切换语言后不能重启应用**（2026-10-11）。系统的资源是解析时按当前
// Configuration 取值的，改语言要么重建 Activity、要么走 AppCompat 的 per-app
// locale —— 前者会让正在跑的接收服务与镜像画面抖一下（进程级引擎虽然活着，但
// Activity 重建期间界面会闪），后者要多引一个依赖并只在新系统上原生支持。
// 自己拿一份表、切换时让 Compose 重新组合，是这里最直接也最可控的做法，
// 而且与 macOS / Windows 两端同一套思路（那边同样不能重启）。
//
// 只放**界面上的字**。日志不在这里 —— 日志（核心与三端打出来的行）一律中文，
// 那是给人排查用的（用户 2026-10-11 的决定）。协议里给手机看的字符串也不在这里
// （设备名、型号、mDNS 的 TXT 记录都是数据，不是界面文字）。
//
// 写成属性而不是 Map<String, String>：少一次查表，而且漏一条或拼错名字在编译期
// 就报，不会等到运行时才发现界面上少了一句话。

package com.adisplay.tv

import android.content.Context

/** 界面语言。只有两种 —— 需求就是中英双语。 */
enum class Language(val tag: String) {
    ZH("zh"),
    EN("en");

    companion object {
        /**
         * 从系统语言推断默认值：**只有明确是英文才用英文，其余一律中文**
         * （用户 2026-10-11：「默认跟随系统语言，非英文一律按中文」）。
         */
        fun fromSystemLanguage(tag: String?): Language {
            if (tag == null) {
                return ZH
            }
            return if (tag.lowercase().startsWith(EN.tag)) EN else ZH
        }

        /** 从存下来的字符串读回，认不出来就用中文。 */
        fun ofStored(value: String?): Language {
            for (language in Language.entries) {
                if (language.tag == value) return language
            }
            return ZH
        }
    }
}

/**
 * 一份语言对应的全部界面文字。
 *
 * 两个实例常驻（[ChineseStrings] / [EnglishStrings]），切换语言时只换引用 ——
 * 不必每次重组都新建对象。
 */
abstract class Strings {

    abstract val language: Language

    // ---- 待机页 ----
    abstract val startReceiving: String
    abstract val stopReceiving: String
    abstract val viewLog: String
    abstract val rename: String
    abstract val mirrorQuality: String
    abstract val qualitySmooth: String
    abstract val qualityBalanced: String
    abstract val qualitySharp: String

    /** 画质那一行下面的说明。三个分辨率从 [QualityPreset] 传进来（不在这里抄一份数）。 */
    abstract fun qualityHint(smooth: String, balanced: String, sharp: String): String

    /** 连接说明的首句：按输入方式不同（遥控器 / 触屏）。 */
    abstract val hintRemoteLead: String
    abstract val hintTouchLead: String
    abstract val hintHowToCast: String
    abstract val localAddressLabel: String
    abstract val hotspotLead: String
    abstract val openHotspotSettings: String
    abstract val hotspotNotFound: String
    abstract val uiLanguage: String

    // ---- 结束投屏后的横幅 ----
    abstract val castingEndedBanner: String
    abstract val resumeCasting: String
    abstract val disconnectCasting: String

    // ---- 播放 / 镜像页 ----
    abstract fun mirroringTitle(deviceName: String): String
    abstract fun playingTitle(deviceName: String): String
    abstract val endCasting: String
    abstract val playbackHint: String
    abstract val rewind10: String
    abstract val forward10: String
    abstract val play: String
    abstract val pause: String
    abstract val logLabel: String
    abstract val buffering: String
    abstract fun bufferingWithSpeed(speed: String): String

    // ---- 改名抽屉 ----
    abstract val deviceNameLabel: String
    abstract val commonNamesLabel: String
    abstract val namePresetLivingRoom: String
    abstract val namePresetBedroom: String
    abstract val namePresetStudy: String
    abstract val save: String
    abstract val cancel: String
    abstract val nameEmpty: String

    // ---- 日志抽屉 ----
    abstract val clearLog: String
    abstract val close: String

    // ---- 状态文字 ----
    abstract val stateStopped: String
    abstract val stateStarting: String
    abstract val stateRunning: String
    abstract val stateStreaming: String
    abstract val stateStopping: String
    abstract val stateError: String
    abstract val coreUnavailable: String
    abstract val createEngineFailed: String

    /** 核心库加载失败时给用户看的那句话（细节由 AdDisplayNative 给出）。 */
    abstract fun coreLoadFailed(detail: String): String

    /**
     * 服务状态的文字。
     *
     * 状态码是核心给的（[ServiceState]），中文原本挂在枚举自己身上做 label ——
     * 现在挂到这里：枚举里存界面文字，双语就没法做（枚举只有一份）。
     */
    fun stateText(state: ServiceState): String = when (state) {
        ServiceState.STOPPED -> stateStopped
        ServiceState.STARTING -> stateStarting
        ServiceState.RUNNING -> stateRunning
        ServiceState.STREAMING -> stateStreaming
        ServiceState.STOPPING -> stateStopping
        ServiceState.ERROR -> stateError
    }

    /**
     * 画质档位的名字（界面用）。
     *
     * 日志里那份中文仍写在 [QualityPreset.label] 上 —— 日志一律中文，不跟着界面走。
     */
    fun qualityLabel(preset: QualityPreset): String = when (preset) {
        QualityPreset.SMOOTH -> qualitySmooth
        QualityPreset.BALANCED -> qualityBalanced
        QualityPreset.SHARP -> qualitySharp
    }

    /** 改名抽屉里的常用名称。 */
    fun namePresets(): List<String> =
        listOf(namePresetLivingRoom, namePresetBedroom, namePresetStudy)

    // ---- 前台服务通知 ----
    abstract val notificationTitle: String
    abstract val notificationText: String
    abstract val notificationStop: String
    abstract val notificationChannelName: String
    abstract val notificationChannelDescription: String
}

object ChineseStrings : Strings() {

    override val language = Language.ZH

    override val startReceiving = "开启接收服务"
    override val stopReceiving = "关闭接收服务"
    override val viewLog = "查看日志"
    override val rename = "修改名称"
    override val mirrorQuality = "镜像画质"
    override val qualitySmooth = "流畅"
    override val qualityBalanced = "均衡"
    override val qualitySharp = "高清"

    override fun qualityHint(smooth: String, balanced: String, sharp: String) =
        "流畅 " + smooth + "｜均衡 " + balanced + "｜高清 " + sharp +
            "\n决定手机镜像时按多大分辨率编码屏幕；改动在手机下次连接时生效，" +
            "当前这一路不受影响。"

    override val hintRemoteLead = "用遥控器选中上方按钮并按「确定」开启接收服务。\n"
    override val hintTouchLead = "点击上方按钮开启接收服务。\n"
    override val hintHowToCast =
        "请让手机与本机连接同一个 Wi-Fi，然后：\n" +
            "iPhone：控制中心 → 屏幕镜像\n" +
            "Android：视频 App 里的「投屏」按钮"
    override val localAddressLabel = "本机地址："
    override val hotspotLead =
        "手机搜不到这台电视？（比如路由器开了 AP 隔离）\n" +
            "可以打开电视的热点，再让手机连上它 —— 在同一张网里就能投。"
    override val openHotspotSettings = "打开热点设置"
    override val hotspotNotFound =
        "这台电视上找不到热点设置页。请到系统的「网络与互联网」设置里手动打开热点，" +
            "再用手机连上它。"
    override val uiLanguage = "界面语言"

    override val castingEndedBanner = "投屏已结束，但手机仍在推流。"
    override val resumeCasting = "继续观看"
    override val disconnectCasting = "断开投屏"

    override fun mirroringTitle(deviceName: String) = deviceName + " · 正在镜像屏幕"
    override fun playingTitle(deviceName: String) = deviceName + " · 正在播放"
    override val endCasting = "结束投屏"
    override val playbackHint = "点画面可再显示控件，按「返回」结束投屏"
    override val rewind10 = "-10秒"
    override val forward10 = "+10秒"
    override val play = "播放"
    override val pause = "暂停"
    override val logLabel = "日志"
    override val buffering = "正在缓冲…"
    override fun bufferingWithSpeed(speed: String) = "正在缓冲　" + speed

    override val deviceNameLabel = "设备名称"
    override val commonNamesLabel = "常用名称"
    override val namePresetLivingRoom = "客厅"
    override val namePresetBedroom = "卧室"
    override val namePresetStudy = "书房"
    override val save = "保存"
    override val cancel = "取消"
    override val nameEmpty = "名称不能为空"

    override val clearLog = "清除日志"
    override val close = "关闭"

    override val stateStopped = "未启动"
    override val stateStarting = "正在启动…"
    override val stateRunning = "正在广播，等待手机连接"
    override val stateStreaming = "正在投屏"
    override val stateStopping = "正在停止…"
    override val stateError = "启动失败"
    override val coreUnavailable = "核心库不可用"
    override val createEngineFailed = "创建引擎失败"

    override fun coreLoadFailed(detail: String) = "核心库加载失败：" + detail

    override val notificationTitle = "正在接收投屏"
    override val notificationText = "手机可以投屏到本机；点这条通知回到界面。"
    override val notificationStop = "停止接收"
    override val notificationChannelName = "接收服务"
    override val notificationChannelDescription = "投屏接收服务运行时的常驻通知"
}

object EnglishStrings : Strings() {

    override val language = Language.EN

    override val startReceiving = "Start receiving"
    override val stopReceiving = "Stop receiving"
    override val viewLog = "View log"
    override val rename = "Rename"
    override val mirrorQuality = "Mirror quality"
    override val qualitySmooth = "Smooth"
    override val qualityBalanced = "Balanced"
    override val qualitySharp = "Sharp"

    override fun qualityHint(smooth: String, balanced: String, sharp: String) =
        "Smooth " + smooth + " | Balanced " + balanced + " | Sharp " + sharp +
            "\nSets how large a screen the phone encodes for mirroring. Takes effect the next " +
            "time it connects; the current session is unaffected."

    override val hintRemoteLead = "Select the button above and press OK to start receiving.\n"
    override val hintTouchLead = "Tap the button above to start receiving.\n"
    override val hintHowToCast =
        "Connect your phone to the same Wi-Fi, then:\n" +
            "iPhone: Control Center → Screen Mirroring\n" +
            "Android: the cast button in a video app"
    override val localAddressLabel = "Local address: "
    override val hotspotLead =
        "Phone can't find this TV? (for example, the router has AP isolation on)\n" +
            "Turn on this TV's hotspot and connect your phone to it — then both are on the " +
            "same network."
    override val openHotspotSettings = "Open hotspot settings"
    override val hotspotNotFound =
        "This TV has no hotspot settings page. Turn the hotspot on from the system's " +
            "\"Network & Internet\" settings, then connect your phone to it."
    override val uiLanguage = "Language"

    override val castingEndedBanner = "Casting ended, but the phone is still streaming."
    override val resumeCasting = "Resume"
    override val disconnectCasting = "Disconnect"

    override fun mirroringTitle(deviceName: String) = deviceName + " · Mirroring screen"
    override fun playingTitle(deviceName: String) = deviceName + " · Playing"
    override val endCasting = "End casting"
    override val playbackHint = "Press any key to show the controls; press Back to end casting"
    override val rewind10 = "-10s"
    override val forward10 = "+10s"
    override val play = "Play"
    override val pause = "Pause"
    override val logLabel = "Log"
    override val buffering = "Buffering…"
    override fun bufferingWithSpeed(speed: String) = "Buffering " + speed

    override val deviceNameLabel = "Device name"
    override val commonNamesLabel = "Common names"
    override val namePresetLivingRoom = "Living Room"
    override val namePresetBedroom = "Bedroom"
    override val namePresetStudy = "Study"
    override val save = "Save"
    override val cancel = "Cancel"
    override val nameEmpty = "Name can't be empty"

    override val clearLog = "Clear log"
    override val close = "Close"

    override val stateStopped = "Stopped"
    override val stateStarting = "Starting…"
    override val stateRunning = "Broadcasting — waiting for a phone"
    override val stateStreaming = "Casting"
    override val stateStopping = "Stopping…"
    override val stateError = "Failed to start"
    override val coreUnavailable = "Core library unavailable"
    override val createEngineFailed = "Failed to create the engine"

    override fun coreLoadFailed(detail: String) = "Failed to load the core library: " + detail

    override val notificationTitle = "Receiving casts"
    override val notificationText = "Phones on this network can cast here. Tap to open the app."
    override val notificationStop = "Stop"
    override val notificationChannelName = "Receiver service"
    override val notificationChannelDescription = "Ongoing notification while the receiver is running"
}

/**
 * 界面语言存在哪、怎么读。
 *
 * 单独抽出来是因为两个地方要用，而且它们的生命周期不一样：
 *   * [EngineModel] —— 界面上的开关改了要写；
 *   * [ReceiverService] —— 那条常驻通知的文字要跟着语言走，而服务可能在 Activity
 *     没起来的时候自己跑着（甚至在开机自启那一路先于界面存在），所以它不能指望
 *     去问 EngineModel。
 *
 * 存 SharedPreferences 而不是核心的配置文件：这是纯界面层的偏好，不该去动 C ABI
 * （与待机页那个「上次服务是不是开着」同一个理由）。
 */
object UiLanguageStore {

    private const val PREFS_NAME = "adisplay-ui"
    private const val KEY_LANGUAGE = "language"

    /** 存下来的语言；没存过就按系统语言定（非英文一律中文）。 */
    fun read(context: Context): Language {
        val prefs = context.applicationContext
            .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val stored = prefs.getString(KEY_LANGUAGE, null)
        if (stored != null) {
            return Language.ofStored(stored)
        }
        return Language.fromSystemLanguage(systemLanguageTag(context))
    }

    fun write(context: Context, language: Language) {
        context.applicationContext
            .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY_LANGUAGE, language.tag)
            .apply()
    }

    /** 当前语言对应的那份文字表。 */
    fun strings(context: Context): Strings =
        if (read(context) == Language.EN) EnglishStrings else ChineseStrings

    /**
     * 系统语言。
     *
     * 用 applicationContext 的 configuration 而不是 Locale.getDefault()：后者是
     * JVM 级的默认值，任何一处代码改过它都会跟着变；configuration 反映的是这台
     * 设备（本应用没覆盖过语言，所以就是系统语言）。
     */
    private fun systemLanguageTag(context: Context): String? {
        val locales = context.applicationContext.resources.configuration.locales
        return if (locales.isEmpty) null else locales[0].language
    }
}
