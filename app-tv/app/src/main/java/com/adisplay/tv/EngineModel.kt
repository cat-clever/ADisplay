package com.adisplay.tv

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

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

    /** 日志行，格式 "[HH:mm:ss] [LEVEL] 正文"，与另外两端一致。 */
    val logs = mutableStateListOf<String>()

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
     * 创建引擎并注册回调。由 Activity 的 onStart 调用。
     *
     * 刻意只创建、不自动启动：装好就一直在广播的话，同一个 Wi-Fi 下
     * 任何人都能直接投过来。文档 2.3 要的是按一下才开。
     */
    fun attach() {
        if (engine != 0L) return

        if (!AdDisplayNative.isAvailable) {
            // 装错 ABI 时最常走到这里。把原因摆在状态文字上，而不是让用户
            // 对着一个没反应的按钮猜是不是网络问题。
            val error = AdDisplayNative.loadError
            statusText = if (error != null) error else "核心库不可用"
            appendLog(LogLevel.ERROR, statusText)
            return
        }

        val handle = AdDisplayNative.nativeCreate(null, null, null)
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

    /** 开启接收服务。 */
    fun startService() {
        val handle = engine
        if (handle == 0L) return
        if (serviceEnabled) return

        serviceEnabled = true
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
        }
    }

    /** 关闭接收服务。未开启时调用是安全的空操作。 */
    fun stopService() {
        val handle = engine
        if (handle == 0L) return
        if (!serviceEnabled) {
            // 状态回调里可能已经把按钮复位了（比如启动失败），这里仍要把锁
            // 兜一次，避免留下一个拿不掉的组播锁。
            releaseMulticastLock()
            return
        }
        serviceEnabled = false
        AdDisplayNative.nativeStop(handle)
        releaseMulticastLock()
    }

    fun toggleService() {
        if (serviceEnabled) stopService() else startService()
    }

    /** 销毁引擎。由 Activity 的 onDestroy 调用。 */
    fun release() {
        stopService()
        val handle = engine
        engine = 0L
        if (handle != 0L) {
            AdDisplayNative.nativeDestroy(handle)
        }
        releaseMulticastLock()
    }

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
            // 播放器还没接（下一批），但地址先记出来 —— 否则用户看到的是
            // 「状态跳到投屏中却什么都没发生」，不知道核心到底收没收到。
            post { appendLog(LogLevel.INFO, "收到媒体地址（会话 " + sessionId + "）：" + url) }
        }

        override fun onPlaybackCommand(sessionId: Int, command: Int, value: Long) {
            post {
                appendLog(
                    LogLevel.DEBUG,
                    "手机发来播放命令 " + command + "，值 " + value + "（会话 " + sessionId + "）"
                )
            }
        }

        override fun onSessionClosed(sessionId: Int, reason: Int) {
            post { appendLog(LogLevel.INFO, "会话 " + sessionId + " 已关闭，原因 " + reason) }
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

    private fun appendLog(level: LogLevel, message: String) {
        logs.add("[" + timeFormat.format(Date()) + "] [" + level.label + "] " + message)
        // 限长：电视上这个页面会一挂就是一整天，不封顶的话日志迟早把内存吃光。
        while (logs.size > MAX_LOG_LINES) {
            logs.removeAt(0)
        }
        // 也往 logcat 打一份：CI 之外真机排障时 adb logcat 往往比看屏幕方便。
        if (level == LogLevel.ERROR) {
            Log.e(TAG, message)
        }
    }
}
