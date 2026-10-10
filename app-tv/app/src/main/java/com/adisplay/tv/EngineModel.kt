package com.adisplay.tv

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.receiveAsFlow

/*
 * 以下三个枚举的数值必须与 include/adisplay/adisplay.h 严格对应 ——
 * 核心库是通过 int 把状态和级别传过来的，对不上就会把 info 显示成 error。
 * 名字改成 Kotlin 风格无所谓，数值一个都不能动。
 */

/** 与 adisplay.h 的 AdServiceState 一一对应。 */
enum class ServiceState(val code: Int, val label: String) {
    STOPPED(0, "未启动"),
    STARTING(1, "正在启动…"),
    RUNNING(2, "正在广播，等待手机连接"),
    STREAMING(3, "正在投屏"),
    STOPPING(4, "正在停止…"),
    ERROR(5, "启动失败");

    companion object {
        fun of(code: Int): ServiceState {
            for (state in ServiceState.entries) {
                if (state.code == code) return state
            }
            // 核心加了新状态而这边还没跟上时按「未启动」处理，不崩。
            return STOPPED
        }
    }
}

/** 与 adisplay.h 的 AdLogLevel 一一对应。label 直接显示在日志行首。 */
enum class LogLevel(val code: Int, val label: String) {
    TRACE(0, "TRACE"),
    DEBUG(1, "DEBUG"),
    INFO(2, "INFO"),
    WARN(3, "WARN"),
    ERROR(4, "ERROR"),
    OFF(5, "OFF");

    companion object {
        fun of(code: Int): LogLevel {
            for (level in LogLevel.entries) {
                if (level.code == code) return level
            }
            return INFO
        }
    }
}

/** 与 adisplay.h 的 AdResult 一一对应。 */
enum class AdResult(val code: Int, val description: String) {
    OK(0, "成功"),
    INVALID_ARG(1, "参数非法"),
    NOT_INITIALIZED(2, "引擎尚未就绪"),
    ALREADY_RUNNING(3, "服务已经在运行"),
    NOT_RUNNING(4, "服务尚未启动"),
    PORT_IN_USE(5, "端口被占用"),
    PERMISSION_DENIED(6, "权限不足"),
    NETWORK(7, "网络错误"),
    UNSUPPORTED(8, "当前设备不支持"),
    BUFFER_TOO_SMALL(9, "缓冲区不足"),
    NOT_FOUND(10, "找不到对应的会话"),
    INTERNAL(11, "核心内部错误");

    companion object {
        fun describe(code: Int): String {
            for (result in AdResult.entries) {
                if (result.code == code) return result.description
            }
            return "未知错误码 " + code
        }
    }
}

/**
 * 与 adisplay.h 的 AdTransportState 一一对应。
 *
 * 数值必须与 UPnP AVTransport 的 TransportState 一致 —— 手机端拿它决定
 * 显示播放还是暂停按钮，对不上就会出现「电视在放、手机显示暂停」。
 * 不带 label：核心那边另有 serviceState / statusText 管界面文字，这份
 * 状态只用来回报，多一份显示文案就多一处会跟手机端对不上的地方。
 */
enum class AdTransportState(val code: Int) {
    NO_MEDIA_PRESENT(0),
    STOPPED(1),
    PLAYING(2),
    PAUSED(3),
    TRANSITIONING(4),
}

/** 与 adisplay.h 的 AdPlaybackCommand 一一对应。value 的含义见各自注释。 */
enum class AdPlaybackCommand(val code: Int) {
    PLAY(0),
    PAUSE(1),

    /** 停止播放，不是结束投屏：媒体还挂着，手机随后可以再发 Play。 */
    STOP(2),

    /** value 是目标位置（毫秒）。 */
    SEEK(3),

    /** value 是 0..100。 */
    SET_VOLUME(4),

    /** value 是 0 或 1。 */
    SET_MUTE(5);

    companion object {
        /**
         * 查不到时返回 null 而不是给个默认值：核心将来加了新命令，这边应该
         * 原样丢掉，而不是拿一个猜出来的命令去动播放器。
         */
        fun of(code: Int): AdPlaybackCommand? {
            for (command in AdPlaybackCommand.entries) {
                if (command.code == code) return command
            }
            return null
        }
    }
}

/**
 * 当前正在播的媒体。
 *
 * 界面靠它是不是 null 决定显示待机页还是播放页；两个字段都是播放页必需的：
 * sessionId 用来回报状态（核心按它找会话），url 是拉流地址。
 */
@Immutable
data class PlayingMedia(val sessionId: Int, val url: String)

/**
 * 一条待执行的播放控制意图。
 *
 * 带上 sessionId：命令是核心在会话上转过来的，页面上那个播放器只认自己的
 * 会话 —— 会话已经换了之后迟到的命令落到新播放器上，会让新视频莫名其妙
 * 地跳一下进度。
 */
data class PlaybackIntent(val sessionId: Int, val command: AdPlaybackCommand, val value: Long)

/**
 * 待机页背后的接收服务。
 *
 * 只做三件事，一行业务判断都没有（协议、会话、播放状态全在核心库里，
 * 这里再判一遍就等于同一套逻辑维护两份）：
 *   1. 起停 castcore：创建 / 启动 / 停止 / 销毁；
 *   2. 把核心的事件回调搬回主线程，摊成 Compose 能观察的状态；
 *   3. 服务运行期间持有 WiFi 组播锁。
 *
 * 线程：核心的回调来自它自己的工作线程。直接在那里改 Compose 状态是能改成，
 * 但多条线程同时改会互相覆盖 —— 日志上最明显，一次贴进来的十几行会少几行，
 * 而那几行往往正是要看的那几行。所以所有状态改动统一 post 到主线程。
 */
class EngineModel(context: Context) {

    companion object {
        /**
         * 日志最多留这么多行。与 macOS / Windows 两端取同一个值 ——
         * 排查时用户会把三端日志对照着看，条数不一样会让人以为漏了什么。
         */
        private const val MAX_LOG_LINES = 500

        /**
         * 每个会话最多替它攒这么多条命令。攒着只是为了跨过「界面切页」那几帧，
         * 给个上限免得某个没人认领的会话把内存占住。
         */
        private const val MAX_PENDING_INTENTS = 32

        // 与 adisplay.h 的 AdStreamKind 对应。这里只关心镜像这一条 ——
        // 「媒体 URL」那条路走 onMediaUrl，不经过会话回调。
        const val STREAM_KIND_MIRROR_VIDEO = 0

        private const val MULTICAST_LOCK_TAG = "adisplay-tv:multicast"

        private const val TAG = "EngineModel"

        private fun defaultDeviceName(): String {
            // 核心库自己也会取一份设备型号，这里只是让界面在后端就绪之前
            // 也有东西可显示，不至于空一块。空字符串在某些低端机型上会出现。
            val model = Build.MODEL
            return if (model == null || model.isBlank()) "Android TV" else model
        }
    }

    private val appContext = context.applicationContext

    /**
     * 配置文件路径 —— 设备名称与 AirPlay 配对密钥都落在它旁边。
     *
     * 必须显式给出，不能用核心的平台默认值：那个默认是 $HOME/.config/adisplay，
     * 而 Android 上 HOME 是 /data，应用写不进去。后果有两条 —— 改完设备名不保存；
     * AirPlay 配对密钥也存不下来，每次投屏 iPhone 都要重新配对一遍（日志里那条
     * 「AirPlay 密钥目录创建失败」的 WARN 说的就是这件事）。
     *
     * filesDir 是 /data/data/<包名>/files，只有本应用能读写，正是该放的地方。
     */
    private val configPath: String =
        File(appContext.filesDir, "adisplay/config.json").absolutePath

    /**
     * mDNS 广播。核心在桌面上自己发（macOS 用系统 Bonjour、Windows 用 DNS-SD），
     * Android 上发不了 —— NsdManager 只在 Java 层，NDK 里没有等价接口。所以这条
     * 路由界面层接手，而**内容**仍由核心给（见 MdnsAdvertiser 的文件头）。
     */
    private val mdns = MdnsAdvertiser(appContext) { message ->
        post { appendLog(LogLevel.INFO, message) }
    }

    /** 日志行，格式 "[HH:mm:ss] [LEVEL] 正文"，与另外两端一致。 */
    val logs = mutableStateListOf<String>()

    /**
     * 正在镜像的会话号。null 表示没在镜像 —— 界面据此切到投屏页。
     *
     * 与「设备已连接」是两件事：用户可能只是从控制中心点开看了一眼，
     * 那时还没有任何一路流。
     */
    var mirrorSessionId by mutableStateOf<Int?>(null)
        private set

    /**
     * 镜像视频帧的落点，由渲染面（MediaCodec）接上。
     *
     * 刻意**不**放进 Compose 状态：每秒几十帧，每帧触发一次重组会把界面压垮。
     * 这和 macOS 那边把帧直接交给显示层、不进 @Published 是同一个道理。
     */
    var mirrorFrameSink: ((ByteArray, Int, Int, Int, Long) -> Unit)? = null

    /**
     * 镜像画面的渲染端。核心交下来的压缩帧经 mirrorFrameSink 到这里，由 MediaCodec
     * 解出来写进 SurfaceView 的 Surface —— 解码与送显都不经过 Compose 的重组。
     */
    val mirrorPlayer = MirrorVideoPlayer { message -> post { appendLog(LogLevel.INFO, message) } }

    /**
     * 镜像伴音的播放端。核心转发的是**压缩**帧（AAC-ELD），这边自己解 ——
     * 注册压缩回调就等于告诉核心「不必解码」，从而避开把 FFmpeg 链进 APK。
     * 见 MirrorAudioPlayer 的文件头。
     */
    val mirrorAudio = MirrorAudioPlayer { message -> post { appendLog(LogLevel.INFO, message) } }

    /** 服务当前的真实状态，来自核心的状态回调。 */
    var serviceState by mutableStateOf(ServiceState.STOPPED)
        private set

    /** 界面上的状态文字。服务起不来时它就是失败原因。 */
    var statusText by mutableStateOf("未启动")
        private set

    /**
     * 界面按钮的开合状态。用户按下就立刻翻转，不等核心回调 ——
     * 核心回调要等端口绑定、mDNS 注册走完才来，那期间按钮如果还停在
     * 「开启接收服务」，用户会以为没按上，然后再按一次。
     */
    var serviceEnabled by mutableStateOf(false)
        private set

    var deviceName by mutableStateOf(defaultDeviceName())
        private set

    var version by mutableStateOf("")
        private set

    /** 本机局域网地址，换行分隔。取不到时为空串。 */
    var localAddresses by mutableStateOf("")
        private set

    /**
     * 当前正在播的媒体，null 表示没有投屏。
     *
     * 界面（StandbyScreen 那一层）只按它切页面，不再自己判断服务状态 ——
     * 两处判就会有两套「什么时候算在投屏」的定义，早晚对不上。
     */
    var playingMedia by mutableStateOf<PlayingMedia?>(null)
        private set

    /**
     * 播放控制意图的通道，由播放页消费。
     *
     * 用带缓冲的 Channel 而不是一个 Compose 状态：命令是有先后的，摊成状态
     * 就只剩最后一条（先 Stop 再 Play 和反过来结果完全不同）。缓冲还兜住了
     * 切页面的那几帧 —— 媒体地址刚回调进来时播放页还没组合出来，这期间到的
     * 命令不该丢。容量给无限：这些命令最多一次一条地进来，攒不出内存问题。
     */
    private val intentChannel = Channel<PlaybackIntent>(Channel.UNLIMITED)

    /** 给播放页的意图流。只有播放页一个消费者。 */
    val playbackIntents: Flow<PlaybackIntent> = intentChannel.receiveAsFlow()

    /**
     * 还没被播放页取走的命令，按会话号攒着。
     *
     * 光靠那条 Channel 会丢命令：发送端是「推地址」和「播放」几乎同时发的
     * （实测相隔 8 毫秒），而界面切到播放页要晚几帧、中间还要新建一个
     * ExoPlayer。那个空档里**上一个会话的收集者可能还活着** —— 它会把这条命令
     * 从 Channel 里收走，然后按「不是我这条会话」丢掉。表现就是：媒体加载完了
     * （时长都读出来了）却停在 00:00 不动。
     *
     * 所以每条命令另外按会话攒一份；播放页挂上来时先取走自己那份补上
     * （见 takePendingIntents）。同一个会话里重复执行 play / pause / seek 是
     * 幂等的，多补一次不会出错。
     *
     * 只在主线程碰它：写入走 post（回调来自核心的工作线程），读取来自界面。
     */
    private val pendingIntents = mutableMapOf<Int, MutableList<PlaybackIntent>>()

    /** 取走某个会话攒下的命令。播放页刚挂上时调用。 */
    fun takePendingIntents(sessionId: Int): List<PlaybackIntent> {
        val taken = pendingIntents.remove(sessionId)
        return if (taken == null) emptyList() else taken
    }

    private fun rememberPendingIntent(intent: PlaybackIntent) {
        val queue = pendingIntents.getOrPut(intent.sessionId) { mutableListOf() }
        queue.add(intent)
        if (queue.size > MAX_PENDING_INTENTS) {
            queue.removeAt(0)
        }
    }

    /**
     * 「第几次投屏」。手机每推一条媒体地址、或者每开一次镜像，它就 +1。
     *
     * 界面靠它判断「这是不是新的一次」：用户手动结束过第 N 次之后，同一条视频
     * 再推过来也算新的一次（地址可能一模一样，光比地址认不出来）。
     */
    var castEpisode by mutableStateOf(0)
        private set

    /**
     * 用户手动结束掉的那一次（castEpisode 的值）。-1 表示没结束过。
     *
     * 为什么要有它：手动结束**不停接收服务** —— 服务一停广播就撤了，手机那边
     * 立刻找不到这台设备，得回去重点一次「开启接收服务」。而核心那边的会话还开
     * 着（JNI 面上没有「只关掉某一个会话」的接口），所以「结束投屏」在界面这一层
     * 就是「回主界面、这次不再显示它」，服务照旧跑着，手机随时可以再投一条。
     */
    var dismissedEpisode by mutableStateOf(-1)
        private set

    /** 手动结束当前这一次投屏：回主界面，但不停接收服务。 */
    /// 用户点「继续观看」：回到投屏页。状态本来就在（dismiss 不清会话），
    /// 复位 dismissedEpisode 并让伴音重新可以出声即可。
    fun resumeCasting() {
        if (!isCastingDismissed()) {
            return
        }
        // 会话本来就没结束（dismissCasting 刻意不清 mirrorSessionId / playingMedia），
        // 所以恢复只要把「不看了」这个标记复位，界面自己就切回播放页。
        dismissedEpisode = -1
        mirrorAudio.resume()
    }

    /**
     * 点「断开投屏」：让核心真的结束这一次会话。
     *
     * AirPlay 那条会让手机停下来；DLNA 只能本地结束并推一条 STOPPED 事件 ——
     * 接收端在 DLNA 里是被动方，协议上命令不了手机。
     */
    fun disconnectCasting() {
        val sessionId = mirrorSessionId ?: playingMedia?.sessionId ?: return
        val handle = engine
        if (handle == 0L) {
            return
        }

        val result = AdDisplayNative.nativeDisconnectSession(handle, sessionId)
        if (result == AdResult.NOT_FOUND.code) {
            // 会话其实已经不在了（手机自己停的，而回调刚到）：自我修复，收掉横幅。
            if (mirrorSessionId == sessionId) {
                mirrorSessionId = null
                mirrorAudio.release()
            }
            if (playingMedia?.sessionId == sessionId) {
                playingMedia = null
            }
            dismissedEpisode = -1
            return
        }
        if (result != AdResult.OK.code) {
            appendLog(LogLevel.WARN, "断开投屏失败：" + AdResult.describe(result))
        }
        // 成功时不自作主张清状态：等 onSessionClosed 收尾 —— 那条路才是
        // 「会话真的没了」的权威。
    }

    fun dismissCasting() {
        dismissedEpisode = castEpisode
        // 光挡住新帧不够：队列里已经排着的那段还会继续响完。要立刻静音就得连
        // 播放线程与解码器一起收掉（release 同时置上挂起标志，之后不会自己重启）。
        mirrorAudio.release()
    }

    /** 当前这一次是不是已经被用户手动结束了。 */
    fun isCastingDismissed(): Boolean {
        return dismissedEpisode == castEpisode
    }

    /** 核心库能不能用。装错 ABI 时为 false，界面据此禁用按钮。 */
    val isNativeAvailable: Boolean
        get() = AdDisplayNative.isAvailable

    private val mainHandler = Handler(Looper.getMainLooper())
    private val timeFormat = SimpleDateFormat("HH:mm:ss", Locale.US)

    /** 引擎句柄。0 表示还没创建（或核心库不可用）。 */
    private var engine = 0L

    /** 服务运行期间持有的组播锁，没拿到时为 null。 */
    private var multicastLock: WifiManager.MulticastLock? = null

    /**
     * 上一次回报播放状态失败的错误码，0（AD_OK）表示上次是成功的。
     *
     * 回报是每秒一次的，失败也会每秒重复一次。只记错误码变化的那一次，
     * 否则日志区一秒一行「回报失败」，真正要看的东西立刻被冲走。
     */
    private var lastReportError = 0

    /**
     * 创建引擎并注册回调。由 Activity 的 onStart 调用。
     *
     * 刻意只创建、不自动启动：装好就一直在广播的话，同一个 Wi-Fi 下
     * 任何人都能直接投过来。文档 2.3 要的是按一下才开。
     */
    fun attach() {
        // 帧的落点接一次即可（这里可能被多次调用，赋值本身是幂等的）。
        mirrorFrameSink = mirrorPlayer::push
        if (engine != 0L) return

        if (!AdDisplayNative.isAvailable) {
            // 装错 ABI 时最常走到这里。把原因摆在状态文字上，而不是让用户
            // 对着一个没反应的按钮猜是不是网络问题。
            val error = AdDisplayNative.loadError
            statusText = if (error != null) error else "核心库不可用"
            appendLog(LogLevel.ERROR, statusText)
            return
        }

        val handle = AdDisplayNative.nativeCreate(null, null, configPath)
        if (handle == 0L) {
            // 句柄为 0 时没有引擎可查 last_error，只能给一句笼统的。
            statusText = "创建引擎失败"
            appendLog(LogLevel.ERROR, "创建引擎失败：核心返回了空句柄")
            return
        }
        engine = handle

        // 先挂回调再读信息：核心在启动阶段打的日志就都收得到，
        // 顺序反了的话头几条（往往最关键）会丢。
        val result = AdDisplayNative.nativeSetCallbacks(handle, callback)
        if (result != AdResult.OK.code) {
            appendLog(LogLevel.ERROR, "注册回调失败：" + AdResult.describe(result))
        }

        version = AdDisplayNative.nativeVersion()
        val name = AdDisplayNative.nativeGetDeviceName(handle)
        if (name.isNotEmpty()) {
            deviceName = name
        }
        localAddresses = AdDisplayNative.nativeLocalAddresses(handle)

        appendLog(LogLevel.INFO, "核心库 " + version + " 就绪，设备名：" + deviceName)
    }

    /**
     * 名称草稿。null 表示没在改名。
     *
     * 电视上没有键盘，改名的入口要显式给出来（见待机页）—— 而 Android TV
     * 在聚焦输入框时会弹出系统输入法，所以这里只需要一个普通输入框。
     */
    var nameDraft by mutableStateOf<String?>(null)
        private set

    fun beginRename() {
        nameDraft = deviceName
    }

    fun cancelRename() {
        nameDraft = null
    }

    fun updateNameDraft(text: String) {
        nameDraft = text
    }

    /** 草稿不合法的原因；空串表示没问题。输入时就校验，而不是等按了保存才报错。 */
    fun nameDraftProblem(): String {
        val draft = nameDraft ?: return ""
        if (draft.isEmpty()) {
            return "名称不能为空"
        }
        return AdDisplayNative.nativeValidateDeviceName(draft)
    }

    /** 保存名称。核心改完会重新注册 mDNS / SSDP，手机端列表几秒内跟着变。 */
    fun applyRename() {
        val draft = nameDraft ?: return
        val problem = if (draft.isEmpty()) "名称不能为空" else
            AdDisplayNative.nativeValidateDeviceName(draft)
        if (problem.isNotEmpty()) {
            appendLog(LogLevel.WARN, problem)
            return
        }
        val handle = engine
        if (handle == 0L) {
            return
        }
        val result = AdDisplayNative.nativeSetDeviceName(handle, draft)
        if (result != AdResult.OK.code) {
            appendLog(LogLevel.ERROR, "改名失败：" + AdResult.describe(result))
            return
        }
        deviceName = draft
        nameDraft = null
        appendLog(LogLevel.INFO, "设备名称已改为「" + draft + "」。手机端列表可能需要几秒刷新。")
    }

    /** 开启接收服务。 */
    fun startService() {
        val handle = engine
        if (handle == 0L) return
        if (serviceEnabled) return

        serviceEnabled = true
        // 服务重新开启，上一次「手动结束」的记忆作废。
        dismissedEpisode = -1
        // 锁要在启动【之前】拿到：SSDP 的 M-SEARCH 是在 start 里开始收的，
        // 晚一步拿锁，第一轮搜索就已经被省电机制滤掉了。
        acquireMulticastLock()

        val result = AdDisplayNative.nativeStart(handle)
        if (result != AdResult.OK.code) {
            // 端口占用这类失败核心自己也会通过 onLog 说明细节，这里再补一条
            // 带错误码的，省得用户只看到一句泛泛的「启动失败」。
            appendLog(LogLevel.ERROR, "启动失败：" + AdResult.describe(result))
            serviceEnabled = false
            releaseMulticastLock()
            return
        }

        // 服务起来了才广播：端口这时才确定，而且「广播了却没东西应答」比
        // 「没广播」更难查 —— 手机上看得到设备、点下去毫无反应。
        mdns.publish(AdDisplayNative.nativeGetAirplayAdvert(handle))

        // 起前台服务保活。手机切到后台、锁屏之后接收不能断，而那时候整个进程
        // 都不再是前台进程，系统随时能把它回收 —— 前台服务那条常驻通知就是
        // 用来换这个的（见 ReceiverService）。
        ReceiverService.start(appContext)
    }

    /** 关闭接收服务。未开启时调用是安全的空操作。 */
    fun stopService() {
        // 前台服务先撤。它只是保活用的外壳，撤晚了会留下一条「正在接收」的通知，
        // 而底下其实已经没有接收了。
        ReceiverService.stop(appContext)

        val handle = engine
        if (handle == 0L) return
        if (!serviceEnabled) {
            // 状态回调里可能已经把按钮复位了（比如启动失败），这里仍要把锁
            // 兜一次，避免留下一个拿不掉的组播锁。
            releaseMulticastLock()
            return
        }
        serviceEnabled = false
        dismissedEpisode = -1
        // 先撤广播再停服务：反过来的话，撤下之前的那一小段时间里手机看到的
        // 是一个已经不应答的设备。
        mdns.withdraw()
        AdDisplayNative.nativeStop(handle)
        releaseMulticastLock()
    }

    fun toggleService() {
        if (serviceEnabled) stopService() else startService()
    }

    /**
     * 把播放页的状态回报给核心。手机端的进度条、音量滑块和播放/暂停按钮
     * 全部以这份回报为准（核心不自己解码，它只能听界面说）。
     *
     * 未知值一律传 -1：positionMs / durationMs 未知是 -1，volume / muted 的 -1
     * 表示「这项没变」。注意 0 是合法值（音量 0、未静音），不能拿它当「不变」，
     * 传 0 会把手机上真实的音量冲掉。
     *
     * 每秒都会调一次，所以这里不做任何日志格式化的开销以外的事。
     *
     * 只从主线程调用（失败时会写日志区，日志区不是线程安全的）。播放页的
     * 协程本来就跑在主线程上，不需要再 post 一次。
     */
    fun reportPlayback(
        sessionId: Int,
        transport: AdTransportState,
        positionMs: Long,
        durationMs: Long,
        volume: Int,
        muted: Int,
    ) {
        val handle = engine
        if (handle == 0L) return

        val result = AdDisplayNative.nativeReportPlayback(
            handle, sessionId, transport.code, positionMs, durationMs, volume, muted
        )
        if (result == AdResult.OK.code) {
            lastReportError = 0
            return
        }
        if (result != lastReportError) {
            lastReportError = result
            appendLog(LogLevel.WARN, "回报播放状态失败：" + AdResult.describe(result))
        }
    }

    /** 销毁引擎。由 Activity 的 onDestroy 调用。 */
    /**
     * 拿一个 WiFi 组播锁。
     *
     * 清单里那条 CHANGE_WIFI_MULTICAST_STATE 只是「允许申请」，不等于已经在收
     * 组播包 —— Android 的省电策略默认把 Wi-Fi 组播包整个滤掉，不持有这个锁
     * 的话 SSDP 的 M-SEARCH 一个字节都到不了应用，表现就是手机翻遍设备列表
     * 也找不到这台电视（而 mDNS 那一半可能还是好的，于是更难判断）。
     *
     * 拿不到 WifiManager 是正常的：电视插网线时系统里可能压根没有 Wi-Fi 服务。
     * 以太网上没有这层过滤，不拿锁也照样收组播，所以这里只记一行日志。
     */
    private fun acquireMulticastLock() {
        if (multicastLock != null) return

        val service = appContext.getSystemService(Context.WIFI_SERVICE)
        if (service !is WifiManager) {
            appendLog(LogLevel.WARN, "没有 Wi-Fi 服务（可能走的是以太网），跳过组播锁")
            return
        }

        try {
            val lock = service.createMulticastLock(MULTICAST_LOCK_TAG)
            // 关掉引用计数：这里只有一对 acquire / release，计数模式下万一
            // 某次 acquire 没配上 release，锁就再也放不掉了，只能重启应用。
            lock.setReferenceCounted(false)
            lock.acquire()
            multicastLock = lock
            appendLog(LogLevel.INFO, "已持有组播锁，SSDP 组播包可以收进来")
        } catch (error: Exception) {
            // 个别机型的 Wi-Fi 服务是个空壳，createMulticastLock 会直接抛。
            // 这时候不收组播也要继续启动 —— 起不来比收不到更难排查。
            appendLog(LogLevel.WARN, "申请组播锁失败：" + error.toString())
        }
    }

    private fun releaseMulticastLock() {
        val lock = multicastLock
        multicastLock = null
        if (lock == null) return
        if (!lock.isHeld) return
        try {
            lock.release()
        } catch (error: Exception) {
            appendLog(LogLevel.WARN, "释放组播锁失败：" + error.toString())
        }
    }

    // ---- 核心回调 ----
    // 全部可能在任意线程上进来，所以一律先转主线程再动状态。

    private val callback = object : AdDisplayNative.AdCallback {

        override fun onStateChanged(state: Int) {
            post { applyState(state) }
        }

        override fun onLog(level: Int, message: String) {
            post { appendLog(LogLevel.of(level), message) }
        }

        override fun onMediaUrl(sessionId: Int, url: String) {
            post {
                // 置上 playingMedia 就等于把界面切到播放页，地址先记出来 ——
                // 否则用户看到的是「状态跳到投屏中却什么都没发生」。
                castEpisode += 1
                // 新一次投屏：解除上一轮「结束投屏」留下的伴音挂起，
                // 不然这一轮会一直没声音。
                mirrorAudio.resume()
                playingMedia = PlayingMedia(sessionId, url)
                appendLog(LogLevel.INFO, "收到媒体地址（会话 " + sessionId + "）：" + url)
            }
        }

        override fun onPlaybackCommand(sessionId: Int, command: Int, value: Long) {
            val parsed = AdPlaybackCommand.of(command)
            post {
                if (parsed == null) {
                    // 核心加了新命令而这边还没跟上，宁可不动播放器也不要猜。
                    appendLog(LogLevel.WARN, "手机发来未知的播放命令 " + command + "，已忽略")
                    return@post
                }
                appendLog(
                    LogLevel.DEBUG,
                    "手机发来播放命令 " + command + "，值 " + value + "（会话 " + sessionId + "）"
                )
                val intent = PlaybackIntent(sessionId, parsed, value)
                // 另外攒一份，跨过「界面切页」那几帧（见 pendingIntents 的说明）。
                post {
                    rememberPendingIntent(intent)
                }
                // 用 trySend 而不是挂起的 send：回调线程不能在这里停住，
                // 缓冲又是无限的，不可能会失败。
                intentChannel.trySend(intent)
            }
        }

        override fun onSessionOpened(sessionId: Int, streamKind: Int) {
            if (streamKind != STREAM_KIND_MIRROR_VIDEO) {
                return
            }
            post {
                castEpisode += 1
                // 新一次投屏：解除上一轮「结束投屏」留下的伴音挂起，
                // 不然这一轮会一直没声音。
                mirrorAudio.resume()
                mirrorSessionId = sessionId
                appendLog(LogLevel.INFO, "iPhone 开始屏幕镜像")
            }
        }

        override fun onMirrorFrame(
            data: ByteArray,
            isH265: Int,
            width: Int,
            height: Int,
            ptsUs: Long
        ) {
            // 不切主线程：每秒几十帧，每帧跳一次会把界面压垮。渲染面自己处理
            // （它把帧喂给 MediaCodec，那是独立线程上的事）。
            val sink = mirrorFrameSink
            if (sink != null) {
                sink(data, isH265, width, height, ptsUs)
            }
        }

        override fun onMirrorAudioFrame(
            data: ByteArray,
            sampleRate: Int,
            channels: Int,
            ptsUs: Long
        ) {
            // 用户已经结束投屏：这一帧不该出声。播放端自己也有一道挂起门禁，
            // 这里是第二道 —— 两道都留着，因为「声音自己回来」是最难解释的一种坏法。
            if (isCastingDismissed()) {
                return
            }
            // 不切主线程，理由同视频帧：每秒约 92 帧，跳一次会让声音断续。
            mirrorAudio.push(data, sampleRate, channels)
        }

        override fun onSessionClosed(sessionId: Int, reason: Int) {
            post {
                appendLog(LogLevel.INFO, "会话 " + sessionId + " 已关闭，原因 " + reason)
                // 只清被关掉的那个会话。核心在多设备抢占时会先关旧会话，
                // 不加比对的话会把刚接管上来的新会话一起抹掉，界面就退回待机页、
                // 把正在放的视频掐了。
                val current = playingMedia
                if (current != null && current.sessionId == sessionId) {
                    playingMedia = null
                }
                // 镜像会话结束同样要退出投屏页，否则电视会停在最后一帧上。
                // 这个会话不会再有人来接命令了，攒着的那份一起丢掉。
                pendingIntents.remove(sessionId)
                if (mirrorSessionId == sessionId) {
                    mirrorSessionId = null
                    mirrorAudio.release()
                }
            }
        }
    }

    private fun post(block: () -> Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            block()
        } else {
            mainHandler.post(block)
        }
    }

    private fun applyState(code: Int) {
        val state = ServiceState.of(code)
        serviceState = state
        statusText = state.label

        // 服务都停了就不可能有媒体在播。这条兜住 onSessionClosed 没到的情况
        // （核心在停服务时不一定逐个会话回调），否则用户按了「结束投屏」
        // 会卡在播放页上出不来。
        if (state == ServiceState.STOPPED || state == ServiceState.ERROR) {
            playingMedia = null
        }

        val handle = engine
        if (handle != 0L) {
            // 本机地址要状态一变就重读一次：核心往往是在 start 里枚举网卡时
            // 才知道自己有哪些地址的，只读一次会一直是空的。
            localAddresses = AdDisplayNative.nativeLocalAddresses(handle)
        }

        if (state == ServiceState.ERROR) {
            // 起不来就退回关闭态：否则按钮停在「关闭接收服务」上，用户按一下
            // 只是走了个 stop，再没法重试一次。
            serviceEnabled = false
            releaseMulticastLock()
            if (handle != 0L) {
                val detail = AdDisplayNative.nativeLastError(handle)
                if (detail.isNotEmpty()) {
                    appendLog(LogLevel.ERROR, detail)
                }
            }
        }
    }

    /**
     * 清空界面上的日志。
     *
     * 核心写的日志文件不动 —— 那份是给事后排查用的，界面上清掉的只是眼前这一屏。
     */
    fun clearLogs() {
        logs.clear()
    }

    private fun appendLog(level: LogLevel, message: String) {
        logs.add("[" + timeFormat.format(Date()) + "] [" + level.label + "] " + message)
        // 限长：电视上这个页面会一挂就是一整天，不封顶的话日志迟早把内存吃光。
        while (logs.size > MAX_LOG_LINES) {
            logs.removeAt(0)
        }
        // 也往 logcat 打一份，而且**不分级别**。
        //
        // 原来只有 ERROR 才进 logcat（注释里写着「比看屏幕方便」，代码却只做了
        // 一级）。结果是真机出问题时只剩两条路：截图，或者看那块一次只显示得下
        // 两行、又不可聚焦因而滚不动的日志区 —— 排查等于没有手段。接上之后
        // adb logcat 就能拿到全量。
        when (level) {
            LogLevel.ERROR -> Log.e(TAG, message)
            LogLevel.WARN -> Log.w(TAG, message)
            LogLevel.DEBUG -> Log.d(TAG, message)
            LogLevel.TRACE -> Log.v(TAG, message)
            else -> Log.i(TAG, message)
        }
    }
}
