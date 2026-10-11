// ADisplay —— Android 电视端的镜像伴音
//
// 与视频那条路同一个分工：能交给平台解的就不在核心解。AAC-ELD 在 Windows 的
// Media Foundation 上不保证支持，所以桌面两端由核心用 FFmpeg 解成 PCM；而
// Android 的 MediaCodec 认它 —— 把 FFmpeg 静态链进 APK 会让包大出上百兆
// （见 core/pipeline/CMakeLists.txt 里的实测），所以电视端走这一条。
//
// 数据流：核心转发 AAC-ELD 裸帧 → MediaCodec 解成 PCM → AudioTrack 播放。
// 全程不碰 Compose 状态：每秒约 92 帧，走状态会把界面压垮。
//
// 线程模型与核心的约定：push 会被核心的工作线程调用，只入队就返回。解码与
// 播放都在一条自己的线程上。

package com.adisplay.tv

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import java.nio.ByteBuffer
import java.util.ArrayDeque
import java.util.concurrent.TimeUnit
import kotlin.concurrent.withLock
import java.util.concurrent.locks.Condition
import java.util.concurrent.locks.ReentrantLock

class MirrorAudioPlayer(private val onNotice: (String) -> Unit) {

    private data class Frame(val data: ByteArray, val sampleRate: Int, val channels: Int)

    private companion object {
        const val MAX_QUEUED_FRAMES = 8
        const val DEQUEUE_TIMEOUT_US = 10_000L
        const val WAIT_FOR_FRAME_MS = 500L

        // csd-0：AAC-ELD 的 AudioSpecificConfig。这四个字节由核心那侧生成
        // （core/pipeline/src/AacEldConfig.cpp），并且有单测拿参考值钉住 ——
        // 这里直接抄过来，而不是在 Kotlin 里再写一遍位拼接：那会成为第二处
        // 可能写错、又同样只表现为「没声音」的地方。
        // 44100 Hz / 立体声 → F8 E8 50 00
        val ELD_CONFIG = byteArrayOf(0xF8.toByte(), 0xE8.toByte(), 0x50.toByte(), 0x00)
    }

    private val lock = ReentrantLock()
    private val hasFrame: Condition = lock.newCondition()
    private val pending = ArrayDeque<Frame>()

    private var running = false

    /**
     * 挂起：停止播放，并且**不再因为收到新帧而自动重启**。
     *
     * 用户点「结束投屏」之后手机还在推流，帧会一直进来。原来 push() 见 !running 就
     * 新起一个播放线程 —— 表现就是「画面关了，声音还在」。这个标志就是那道门禁，
     * 要重新出声必须显式 resume()。
     */
    private var suspended = false
    private var worker: Thread? = null

    // 只有 worker 线程碰。
    private var codec: MediaCodec? = null
    private var track: AudioTrack? = null

    /** 收一帧压缩的 AAC-ELD。由核心的工作线程调用。 */
    fun push(data: ByteArray, sampleRate: Int, channels: Int) {
        lock.withLock {
            // 挂起期间连帧都不收：既省掉解码，也彻底堵死「自动复活」这条路。
            if (data.isEmpty() || suspended) {
                return
            }
            if (!running) {
                running = true
                val thread = Thread { loop() }
                thread.name = "MirrorAudio"
                thread.start()
                worker = thread
            }
            while (pending.size >= MAX_QUEUED_FRAMES) {
                pending.pollFirst()
            }
            pending.addLast(Frame(data, sampleRate, channels))
            hasFrame.signalAll()
        }
    }

    /// 解除挂起：下一次收到帧时重新把解码与播放建起来。用于「继续观看」。
    fun resume() {
        lock.withLock { suspended = false }
    }

    fun release() {
        lock.withLock {
            running = false
            // release 的语义是「停止且不许自己重启」，要重新出声必须显式 resume。
            suspended = true
            pending.clear()
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
                // 同 MirrorVideoPlayer：让 withLock 返回取到的东西，而不是往
                // 外层变量里写 —— 后者会让 Kotlin 拒绝做智能转换。
                val frame = lock.withLock {
                    if (!running) {
                        return
                    }
                    val next = pending.pollFirst()
                    if (next == null) {
                        hasFrame.await(WAIT_FOR_FRAME_MS, TimeUnit.MILLISECONDS)
                    }
                    next
                }

                if (frame == null) {
                    // 没有新帧也要把解码器的输出排空，否则它的缓冲会被占满。
                    drain(info)
                    continue
                }

                ensureCodec(frame)
                feed(frame)
                drain(info)
            }
        } catch (error: Exception) {
            onNotice("镜像伴音：解码线程出错 —— " + (error.message ?: error.toString()))
        } finally {
            releaseCodec()
        }
    }

    private fun ensureCodec(frame: Frame) {
        if (codec != null) {
            return
        }
        try {
            val format = MediaFormat.createAudioFormat(
                MediaFormat.MIMETYPE_AUDIO_AAC,
                frame.sampleRate,
                frame.channels
            )
            format.setByteBuffer("csd-0", ByteBuffer.wrap(ELD_CONFIG))

            val created = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
            created.configure(format, null, null, 0)
            created.start()
            codec = created
            onNotice(
                "镜像伴音：解码器已就绪（AAC-ELD " + frame.sampleRate + " Hz / " +
                    frame.channels + " 声道）"
            )
        } catch (error: Exception) {
            // 少数电视盒子的解码器不认 AAC-ELD。这时说清楚是设备不支持，
            // 而不是让用户以为是自己哪里没设对。
            onNotice("镜像伴音：这台设备的解码器不支持 —— " + (error.message ?: error.toString()))
            lock.withLock { running = false }
        }
    }

    private fun feed(frame: Frame) {
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
        current.queueInputBuffer(index, 0, frame.data.size, 0, 0)
    }

    private fun drain(info: MediaCodec.BufferInfo) {
        val current = codec ?: return
        while (true) {
            val index = current.dequeueOutputBuffer(info, 0)
            if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                // 解码器此刻才告诉我们输出格式（采样率、声道、位深），
                // AudioTrack 必须按它来建 —— 猜错就是噪音或没声音。
                createTrack(current.outputFormat)
                continue
            }
            if (index < 0) {
                return
            }
            val buffer = current.getOutputBuffer(index)
            val sink = track
            if (buffer != null && sink != null && info.size > 0) {
                buffer.position(info.offset)
                buffer.limit(info.offset + info.size)
                sink.write(buffer, info.size, AudioTrack.WRITE_BLOCKING)
            }
            current.releaseOutputBuffer(index, false)
        }
    }

    private fun createTrack(format: MediaFormat) {
        if (track != null) {
            return
        }
        val sampleRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
        val channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
        val mask =
            if (channels == 1) AudioFormat.CHANNEL_OUT_MONO else AudioFormat.CHANNEL_OUT_STEREO
        val bytes = AudioTrack.getMinBufferSize(
            sampleRate,
            mask,
            AudioFormat.ENCODING_PCM_16BIT
        )
        if (bytes <= 0) {
            onNotice("镜像伴音：这台设备给不出可用的音频缓冲，这一路没有声音")
            return
        }
        track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(sampleRate)
                    .setChannelMask(mask)
                    .build()
            )
            .setBufferSizeInBytes(bytes * 2)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        track?.play()
        onNotice("镜像伴音：已开始播放（" + sampleRate + " Hz / " + channels + " 声道）")
    }

    private fun releaseCodec() {
        track?.let {
            try {
                it.stop()
            } catch (ignored: Exception) {
                // 没在播时 stop 会抛，直接释放即可。
            }
            it.release()
        }
        track = null

        val current = codec ?: return
        codec = null
        try {
            current.stop()
        } catch (ignored: Exception) {
            // 同上。
        }
        current.release()
    }
}
