// ADisplay —— Android 电视端的镜像渲染
//
// 与 macOS 的 AVSampleBufferDisplayLayer 是同一个分工：核心不养解码器，压缩帧
// 直接交给平台的解码器。Android 的入口是 MediaCodec —— 它解出来直接写进
// SurfaceView 的 Surface，中间不需要我们碰像素。
//
// Android 有一点比 macOS 省事：MediaCodec 直接吃 **Annex B**（起始码分隔），
// 而核心交过来的正是 Annex B，所以这边不需要 macOS 那样的 AVCC 转换。
//
// 线程模型与核心的约定：push 会被核心的工作线程调用，只入队就返回（每秒几十帧，
// 谁都不能在那里停住）；真正的解码与送显在一条自己的线程上，MediaCodec 的
// dequeue/queue 都在那条线程里做。
//
// 参数集（SPS/PPS，H.265 还有 VPS）必须交给解码器：核心保证每个关键帧都带着
// 它们（见 protocols/airplay/src/MirrorNalu.cpp），所以第一帧就一定取得到。

package com.adisplay.tv

import android.graphics.ImageFormat
import android.media.ImageReader
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.view.Surface
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.util.ArrayDeque
import java.util.concurrent.TimeUnit
// withLock 在 kotlin.concurrent 里，不在默认导入的那几个包里。
import kotlin.concurrent.withLock
import java.util.concurrent.locks.Condition
import java.util.concurrent.locks.ReentrantLock

class MirrorVideoPlayer(private val onNotice: (String) -> Unit) {

    private data class Frame(
        val data: ByteArray,
        val isH265: Boolean,
        val width: Int,
        val height: Int,
        val ptsUs: Long,
    )

    private companion object {
        // 队列上限。镜像宁可按最新画面走，也不要越积越久 —— 积压只会让画面
        // 越来越滞后。落后时丢最旧的一帧。
        const val MAX_QUEUED_FRAMES = 8
        const val DEQUEUE_TIMEOUT_US = 10_000L
        const val WAIT_FOR_FRAME_MS = 500L
    }

    // 用 ReentrantLock + Condition 而不是 Object 的 wait/notify：Kotlin 里
    // Any 不暴露那两个方法，得处处强制转换成 java.lang.Object，很容易漏一处。
    private val lock = ReentrantLock()
    private val hasFrame: Condition = lock.newCondition()
    private val pending = ArrayDeque<Frame>()

    /**
     * 累计丢掉多少帧（队列溢出）。只用来报警与排查，只在持锁时读写。
     *
     * 它一涨，就说明「画面卡一下然后突然加速」是**我们**这层丢帧造成的（只画最新的，
     * 中间那段被丢掉，看起来就是跳了一下）；它不涨而画面仍然卡，那就是上游没给帧。
     */
    private var droppedFrames = 0L

    private var surface: Surface? = null
    private var worker: Thread? = null
    private var running = false

    /**
     * 解码器报出来的画面尺寸。null 表示还没解出第一帧。
     *
     * 界面层拿它做信箱式留边 —— 少了这一步，SurfaceView 铺满父容器，
     * 而解码器是**按 Surface 的宽高比拉伸画面**的，比例就错了（竖屏手机上
     * 会把画面横向拉宽四成）。它是 Compose 状态，界面据此重排。
     */
    var videoSize by mutableStateOf<Pair<Int, Int>?>(null)
        private set

    // 只有 worker 线程碰这几个。
    private var codec: MediaCodec? = null

    /** 解码器当前绑的输出面。用来判断要不要换面（见 rebindCodecSurface）。 */
    private var codecSurface: Surface? = null

    /** 占位输出面（见 parkCodecOnPlaceholder）：面没了时把解码器的输出切到它。 */
    private var placeholder: Surface? = null

    /** 占位面的持有者。不能让它被回收 —— 回收了 Surface 就没了。 */
    private var placeholderReader: ImageReader? = null

    /** 「参数集不全」报过多少次。这条以前每帧都打，一次会话能刷几百行（见下面的限流）。 */
    private var parameterSetMisses = 0L

    private var codecH265 = false
    private var codecWidth = 0
    private var codecHeight = 0

    /** 渲染面就绪。可以重复调用（界面重建时会再来一次）。 */
    fun attach(surface: Surface) {
        lock.withLock {
            this.surface = surface
            if (!running) {
                running = true
                val thread = Thread { loop() }
                thread.name = "MirrorVideo"
                thread.start()
                worker = thread
            }
            hasFrame.signalAll()
        }
    }

    /** 渲染面没了（界面切走、Activity 暂停）。解码器一并收掉，下次重建。 */
    /**
     * 渲染面没了（界面切走、**Activity 暂停** —— 例如点「导出日志」会打开系统的文件选择器，
     * 那时我们会被切到后台）。
     *
     * **刻意不释放解码器**：一释放，回来就得重建，而重建必须等发送端下一个 I 帧 ——
     * iOS 插关键帧极稀（实测一份 1956 帧的日志里只有 1 个关键帧），画面于是会冻很久。
     * 用户 2026-10-11 点「导出日志」时踩到的就是这个：日志里
     * `H.264 参数集不全，等下一个关键帧` 刷了 521 次。
     *
     * 这里只把「没有面」这件事记下来，具体动作交给 worker 线程（它一个人持有解码器）：
     * 下一轮它会把输出切到占位面（见 parkCodecOnPlaceholder），回来时再切回真面
     * （见 rebindCodecSurface）—— 全程不重建，回来第一个 P 帧就能画出来。
     */
    fun detach() {
        lock.withLock { surface = null }
    }

    /** 会话结束、重新开始时清掉上一路的尺寸，免得沿用旧比例。 */
    fun forgetVideoSize() {
        videoSize = null
    }

    /** 收一帧（Annex B，核心交过来的原始形态）。由核心的工作线程调用。 */
    fun push(data: ByteArray, isH265: Int, width: Int, height: Int, ptsUs: Long) {
        var dropped = false
        lock.withLock {
            if (!running || data.isEmpty()) {
                return
            }
            while (pending.size >= MAX_QUEUED_FRAMES) {
                pending.pollFirst()
                droppedFrames += 1
                dropped = true
            }
            pending.addLast(Frame(data, isH265 != 0, width, height, ptsUs))
            hasFrame.signalAll()
        }

        // 丢帧要能看见。以前它是完全静默的，于是「画面卡一下然后突然加速」这种事只能靠猜：
        // 到底是**我们**解码跟不上（丢帧、只画最新的），还是**上游**压根没给帧（iOS 的镜像
        // 管线顿了一下再补一波）。日志里有没有这一行，就是这两者的分界。
        // 调用放在锁外 —— 持锁调用户代码是本项目一直避免的事。
        if (dropped && (droppedFrames == 1L || droppedFrames % 50L == 0L)) {
            onNotice(
                "镜像渲染：队列满了，累计丢 " + droppedFrames +
                    " 帧（解码跟不上到达速度 —— 画面会卡一下再往前跳）"
            )
        }
    }

    fun release() {
        lock.withLock {
            running = false
            pending.clear()
            droppedFrames = 0L
            surface = null
            hasFrame.signalAll()
        }
        // 清掉尺寸：下一个会话可能是别的分辨率，沿用旧比例就会留错边。
        forgetVideoSize()
        val thread = worker
        if (thread != null) {
            try {
                thread.join(1000)
            } catch (interrupted: InterruptedException) {
                Thread.currentThread().interrupt()
            }
        }
        worker = null
        releaseCodec()
    }

    // ---------------------------------------------------------------------
    // worker 线程
    // ---------------------------------------------------------------------

    private fun loop() {
        val info = MediaCodec.BufferInfo()
        try {
            while (true) {
                // 让 withLock 把取到的东西**返回**，而不是往外层变量里写：
                // 在 lambda 里赋值的局部变量，Kotlin 之后不肯做智能转换
                // （"captured by a changing closure"）。
                val picked = lock.withLock {
                    if (!running) {
                        return
                    }
                    val next = pending.pollFirst()
                    if (next == null) {
                        hasFrame.await(WAIT_FOR_FRAME_MS, TimeUnit.MILLISECONDS)
                    }
                    Pair(next, surface)
                }
                val frame = picked.first
                val target = picked.second

                if (target == null) {
                    // 没有渲染面：把解码器的输出切到占位面（它继续解码，输出被丢掉）。
                    // 既不能让它去渲染那个已经销毁的 surface，也不能把解码器释放掉 ——
                    // 后者会让画面一直冻着等下一个 I 帧（见 detach 的注释）。
                    parkCodecOnPlaceholder()
                    drain(info)
                    continue
                }
                if (frame == null) {
                    // 没有新帧也要把解码器的输出排空，否则它的缓冲会被占满。
                    drain(info)
                    continue
                }

                ensureCodec(frame, target)
                feed(frame, info)
                drain(info)
            }
        } catch (error: Exception) {
            onNotice("镜像渲染：解码线程出错 —— " + (error.message ?: error.toString()))
        } finally {
            releaseCodec()
        }
    }

    /** 建解码器。编码格式、尺寸或渲染面变了就重建 —— MediaCodec 一旦 configure 就绑死了这些。 */
    private fun ensureCodec(frame: Frame, target: Surface) {
        if (codec != null &&
            codecH265 == frame.isH265 &&
            codecWidth == frame.width &&
            codecHeight == frame.height
        ) {
            // 解码器还能用，只需确认它绑的是当前这块面（暂停过、切过页面、换过面）。
            rebindCodecSurface(target)
            return
        }
        releaseCodec()

        val mime = if (frame.isH265) MediaFormat.MIMETYPE_VIDEO_HEVC else MediaFormat.MIMETYPE_VIDEO_AVC
        // 尺寸取发送端报的；拿不到就先用 1920x1080 顶着，解码器会按 SPS 里的实际
        // 尺寸输出（这两个值只影响缓冲分配）。
        val width = if (frame.width > 0) frame.width else 1920
        val height = if (frame.height > 0) frame.height else 1080

        val format = MediaFormat.createVideoFormat(mime, width, height)
        val sets = parameterSets(frame.data, frame.isH265)
        if (frame.isH265) {
            // HEVC 的三个要分开给：csd-0 是 VPS、csd-1 是 SPS、csd-2 是 PPS。
            val vps = sets.firstOrNull { typeOf(it, true) == 32 }
            val sps = sets.firstOrNull { typeOf(it, true) == 33 }
            val pps = sets.firstOrNull { typeOf(it, true) == 34 }
            if (vps == null || sps == null || pps == null) {
                noteParameterSetMiss()
                return
            }
            format.setByteBuffer("csd-0", java.nio.ByteBuffer.wrap(vps))
            format.setByteBuffer("csd-1", java.nio.ByteBuffer.wrap(sps))
            format.setByteBuffer("csd-2", java.nio.ByteBuffer.wrap(pps))
        } else {
            // AVC 要把 SPS 与 PPS 拼在一起给 csd-0（每个前面带起始码）。
            val sps = sets.firstOrNull { typeOf(it, false) == 7 }
            val pps = sets.firstOrNull { typeOf(it, false) == 8 }
            if (sps == null || pps == null) {
                noteParameterSetMiss()
                return
            }
            format.setByteBuffer("csd-0", java.nio.ByteBuffer.wrap(startCode() + sps + startCode() + pps))
        }

        val created = MediaCodec.createDecoderByType(mime)
        created.configure(format, target, null, 0)
        created.start()
        codec = created
        codecSurface = target
        codecH265 = frame.isH265
        codecWidth = frame.width
        codecHeight = frame.height
        onNotice("镜像渲染：解码器已就绪（" + (if (frame.isH265) "H.265" else "H.264") +
            " " + width + "x" + height + "）")
    }

    private fun feed(frame: Frame, info: MediaCodec.BufferInfo) {
        val current = codec ?: return
        val index = current.dequeueInputBuffer(DEQUEUE_TIMEOUT_US)
        if (index < 0) {
            return   // 解码器暂时没有空缓冲：丢这一帧，等下一帧
        }
        val buffer = current.getInputBuffer(index)
        if (buffer == null) {
            return
        }
        buffer.clear()
        buffer.put(frame.data)
        // 时间戳用发送端给的微秒值 —— MediaCodec 的约定就是微秒，正好对得上。
        current.queueInputBuffer(index, 0, frame.data.size, frame.ptsUs, 0)
    }

    private fun drain(info: MediaCodec.BufferInfo) {
        val current = codec ?: return
        while (true) {
            val index = current.dequeueOutputBuffer(info, 0)
            if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                // 解码器此刻才报出真正的画面尺寸（可能与 configure 时给的
                // 建议值不同）。界面用它做留边。
                val format = current.outputFormat
                val width = format.getInteger(MediaFormat.KEY_WIDTH)
                val height = format.getInteger(MediaFormat.KEY_HEIGHT)
                if (width > 0 && height > 0) {
                    videoSize = Pair(width, height)
                }
                continue
            }
            if (index < 0) {
                return   // 还没数据，下一轮再来
            }
            // true = 直接送到 Surface 上显示。
            current.releaseOutputBuffer(index, true)
        }
    }

    /// 解码器还在、但绑的面换了：切到新面，而不是重建。
    ///
    /// **只在 worker 线程调用** —— 解码器的所有操作都必须留在同一个线程上，
    /// `setOutputSurface` 也一样（它和取缓冲并发会在真机上抛 IllegalStateException）。
    private fun rebindCodecSurface(target: Surface) {
        val current = codec
        if (current == null || codecSurface === target) {
            return
        }
        try {
            current.setOutputSurface(target)
            codecSurface = target
        } catch (error: Exception) {
            // 换不了就退回重建（老行为）：画面要等下一个 I 帧，但至少不会画到错的面上去。
            releaseCodec()
        }
    }

    /// 没有渲染面时，把解码器停到占位面上（继续解码、输出丢掉）。只在 worker 线程调用。
    private fun parkCodecOnPlaceholder() {
        val current = codec
        if (current == null) {
            return
        }
        val parked = placeholderSurface()
        if (parked == null) {
            // 拿不到占位面：退回释放解码器 —— 总比让它渲染到已销毁的面上强。
            releaseCodec()
            return
        }
        rebindCodecSurface(parked)
    }

    /// 占位输出面：一个很小的 ImageReader 面，只用来"接住"解码器的输出。
    ///
    /// 用 `ImageFormat.PRIVATE`（不透明）—— 视频解码器的输出正好是这种。拿不到就返回 null，
    /// 调用方退回老行为。回调里必须把图像取走并关掉，否则生产端（解码器）会堵住。
    private fun placeholderSurface(): Surface? {
        val existing = placeholder
        if (existing != null) {
            return existing
        }
        try {
            val reader = ImageReader.newInstance(320, 180, ImageFormat.PRIVATE, 2)
            reader.setOnImageAvailableListener({ source ->
                try {
                    val image = source.acquireLatestImage()
                    if (image != null) {
                        image.close()
                    }
                } catch (error: Exception) {
                    // 取不到就算了：这一路本来就是"接住再丢掉"。
                }
            }, null)
            placeholderReader = reader
            placeholder = reader.surface
            return placeholder
        } catch (error: Exception) {
            return null
        }
    }

    /// 「参数集不全」的限流提示：首帧报一次，之后每 100 次报一次。
    ///
    /// 这条以前**每帧都打** —— 解码器建不起来时一秒钟就能刷几十行，把日志环冲光
    /// （用户 2026-10-11 那份日志里它出现了 521 次）。现象本身要紧（画面就是冻着的），
    /// 但不需要每帧说一遍。
    private fun noteParameterSetMiss() {
        parameterSetMisses += 1
        if (parameterSetMisses == 1L || parameterSetMisses % 100L == 0L) {
            onNotice(
                "镜像渲染：参数集不全，等下一个关键帧（累计 " + parameterSetMisses +
                    " 帧）—— 画面会停到发送端插入关键帧为止"
            )
        }
    }

    private fun releaseCodec() {
        codecSurface = null
        placeholderReader?.close()
        placeholderReader = null
        placeholder = null
        val current = codec ?: return
        codec = null
        try {
            current.stop()
        } catch (ignored: Exception) {
            // stop 抛异常说明它已经不在跑，直接释放即可。
        }
        try {
            current.release()
        } catch (ignored: Exception) {
            // 释放失败也没有补救手段。
        }
    }

    // ---------------------------------------------------------------------
    // Annex B 解析
    // ---------------------------------------------------------------------

    private fun startCode(): ByteArray = byteArrayOf(0, 0, 0, 1)

    private fun typeOf(unit: ByteArray, isH265: Boolean): Int {
        if (unit.isEmpty()) {
            return -1
        }
        val header = unit[0].toInt() and 0xFF
        return if (isH265) (header shr 1) and 0x3F else header and 0x1F
    }

    /** 按起始码切出 NALU（不含起始码）。与核心那侧 MirrorNalu 的规则一致。 */
    private fun parameterSets(data: ByteArray, isH265: Boolean): List<ByteArray> {
        val units = ArrayList<ByteArray>()
        var index = 0
        var start = -1
        while (index + 3 <= data.size) {
            if (data[index] == 0.toByte() && data[index + 1] == 0.toByte()) {
                val length = when {
                    data[index + 2] == 1.toByte() -> 3
                    index + 4 <= data.size &&
                        data[index + 2] == 0.toByte() && data[index + 3] == 1.toByte() -> 4
                    else -> 0
                }
                if (length > 0) {
                    if (start >= 0 && index > start) {
                        units.add(data.copyOfRange(start, index).trimTrailingZeros())
                    }
                    start = index + length
                    index += length
                    continue
                }
            }
            index++
        }
        if (start in 0 until data.size) {
            units.add(data.copyOfRange(start, data.size).trimTrailingZeros())
        }
        return units.filter { typeOf(it, isH265) >= 0 }
    }

    private fun ByteArray.trimTrailingZeros(): ByteArray {
        var end = size
        while (end > 0 && this[end - 1] == 0.toByte()) {
            end--
        }
        return if (end == size) this else copyOfRange(0, end)
    }
}
