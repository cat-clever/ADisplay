package com.adisplay.tv

import android.util.Log

/**
 * castcore 的 JNI 入口。
 *
 * 这一层薄到没有逻辑：只是把 adisplay_jni.cpp 里那几个
 * Java_com_adisplay_tv_AdDisplayNative_* 符号声明出来。方法名、参数类型，
 * 以及下面 AdCallback 的五个方法签名，都是 JNI 那边用 GetMethodID 逐字
 * 查找的 —— 改任何一处，nativeSetCallbacks 就会返回错误码，核心的事件
 * 一个都到不了界面（只在 logcat 里留一行签名不匹配）。
 *
 * 这些必须是【实例方法】：JNI 的第二个参数写的是 jobject 而不是 jclass。
 * 所以这里用 object 里的普通 external fun，【不要】加 @JvmStatic ——
 * 静态化之后符号形状对不上，注册一律失败。
 */
object AdDisplayNative {

    private const val TAG = "AdDisplayNative"

    /**
     * 核心库事件回调。
     *
     * 回调可能在 castcore 的任意线程上进来（网络线程、解码线程，甚至是调用
     * nativeStart 的那个线程），实现方必须自己切回主线程再碰界面状态。
     */
    interface AdCallback {
        fun onStateChanged(state: Int)
        fun onLog(level: Int, message: String)
        fun onMediaUrl(sessionId: Int, url: String)
        fun onPlaybackCommand(sessionId: Int, command: Int, value: Long)
        fun onSessionClosed(sessionId: Int, reason: Int)

        /**
         * 会话建立。[streamKind] 见 C 的 AdStreamKind —— 0 是镜像视频。
         *
         * 界面层据此切到投屏页：在此之前「设备连上了」和「开始投屏了」是两件事，
         * 用户可能只是从控制中心看了一眼。
         */
        fun onSessionOpened(sessionId: Int, streamKind: Int)

        /**
         * 一帧镜像视频（**压缩**，Annex B —— 每个 NALU 前是 00 00 01 起始码）。
         *
         * 解码交给 MediaCodec：Android 的解码器直接吃 Annex B，所以这边不像 macOS
         * 那样还要转成 AVCC。data 只在本次调用期间有效，需要留存必须自己拷走。
         */
        fun onMirrorFrame(data: ByteArray, isH265: Int, width: Int, height: Int, ptsUs: Long)
    }

    /** 核心库是否可用。为 false 时下面所有 native* 都不能调。 */
    val isAvailable: Boolean

    /** 加载失败的说明，成功时为 null，可以直接显示给用户。 */
    val loadError: String?

    init {
        var failure: String? = null
        try {
            System.loadLibrary("adisplay_jni")
        } catch (error: Throwable) {
            // 这里必须把异常吞掉，不能让类初始化抛出去。电视上装错 ABI 是
            // 真实会发生的（arm64 的包侧载到 32 位老盒子上），那会儿
            // loadLibrary 直接抛 UnsatisfiedLinkError，应用一进桌面就闪退，
            // 用户只看到一个「点了没反应」的图标 —— 比在界面上写一句
            // 「核心库加载失败」难查得多。两个 .so 任何一个缺失也走到这里。
            val detail = error.message
            failure = "核心库加载失败：" +
                (if (detail != null) detail else error.javaClass.simpleName)
            Log.e(TAG, "加载 libadisplay_jni.so 失败", error)
        }
        isAvailable = failure == null
        loadError = failure
    }

    /** 核心库版本号，形如 "0.4.4"。 */
    external fun nativeVersion(): String

    /**
     * 创建引擎，返回句柄，0 表示失败。
     *
     * 三个字符串传 null 表示「用核心的默认值」：设备名取设备型号、
     * 日志只走回调不落盘（电视上没法从应用目录里取文件，落盘没意义）。
     */
    external fun nativeCreate(deviceName: String?, logFilePath: String?, configFilePath: String?): Long

    /** 注册回调。必须在 nativeStart 之前调用，返回 0（AD_OK）表示成功。 */
    external fun nativeSetCallbacks(handle: Long, callback: AdCallback?): Int

    /** 启动接收服务，返回 AdResult 的数值。 */
    external fun nativeStart(handle: Long): Int

    /** 停止接收服务。未启动时调用是安全的空操作。 */
    external fun nativeStop(handle: Long)

    /** 销毁引擎（会先隐式停止）。调用之后句柄作废。 */
    external fun nativeDestroy(handle: Long)

    /** 当前状态，取值见 ServiceState。 */
    external fun nativeGetState(handle: Long): Int

    /** 最近一次失败的详细描述，没有时返回空串。 */
    external fun nativeLastError(handle: Long): String

    /** 设备唯一标识。改名不影响它（文档 2.4）。 */
    external fun nativeDeviceId(handle: Long): String

    /** 本机局域网地址，换行分隔，供待机页显示。 */
    external fun nativeLocalAddresses(handle: Long): String

    /** 当前设备名称。 */
    external fun nativeGetDeviceName(handle: Long): String

    /** 修改设备名称，即时生效（核心会重新注册 mDNS / SSDP）。 */
    external fun nativeSetDeviceName(handle: Long, name: String): Int

    /** 把界面播放器的状态回报给核心，手机端的进度条与音量靠它更新。 */
    external fun nativeReportPlayback(
        handle: Long,
        sessionId: Int,
        transportState: Int,
        positionMs: Long,
        durationMs: Long,
        volume: Int,
        muted: Int,
    ): Int
}
