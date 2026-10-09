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

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.view.Surface
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

    private var surface: Surface? = null
    private var worker: Thread? = null
    private var running = false

    // 只有 worker 线程碰这几个。
    private var codec: MediaCodec? = null
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
    fun detach() {
        lock.withLock { surface = null }
        releaseCodec()
    }

    /** 收一帧（Annex B，核心交过来的原始形态）。由核心的工作线程调用。 */
    fun push(data: ByteArray, isH265: Int, width: Int, height: Int, ptsUs: Long) {
        lock.withLock {
            if (!running || data.isEmpty()) {
                return
            }
            while (pending.size >= MAX_QUEUED_FRAMES) {
                pending.pollFirst()
            }
            pending.addLast(Frame(data, isH265 != 0, width, height, ptsUs))
            hasFrame.signalAll()
        }
    }

    fun release() {
        lock.withLock {
            running = false
            pending.clear()
            surface = null
            hasFrame.signalAll()
        }
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
                var frame: Frame? = null
                var target: Surface? = null
                lock.withLock {
                    if (!running) {
                        return
                    }
                    target = surface
                    frame = pending.pollFirst()
                    if (frame == null) {
                        hasFrame.await(WAIT_FOR_FRAME_MS, TimeUnit.MILLISECONDS)
                    }
                }

                if (target == null) {
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
                onNotice("镜像渲染：H.265 参数集不全，等下一个关键帧")
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
                onNotice("镜像渲染：H.264 参数集不全，等下一个关键帧")
                return
            }
            format.setByteBuffer("csd-0", java.nio.ByteBuffer.wrap(startCode() + sps + startCode() + pps))
        }

        val created = MediaCodec.createDecoderByType(mime)
        created.configure(format, target, null, 0)
        created.start()
        codec = created
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
            if (index < 0) {
                return   // 还要么没数据、要么换了格式，都会在下一轮再来
            }
            // true = 直接送到 Surface 上显示。
            current.releaseOutputBuffer(index, true)
        }
    }

    private fun releaseCodec() {
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
