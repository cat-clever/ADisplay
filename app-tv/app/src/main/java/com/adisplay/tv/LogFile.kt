// ADisplay —— 日志落盘
//
// 为什么落盘：界面上的日志环只有 500 行，而 AirPlay 那类逐包日志一抖就是每秒上千行
// （v1.0.5 起才压掉）、再加上我们自己的诊断行，几百毫秒就能把它填满 —— 用户点
// 「复制全部」拿到的往往不是想看的那一段。落成文件之后完整记录都在，可以整个取走、
// 在电脑上慢慢看。
//
// 位置：应用自己的外部目录 Android/data/<包名>/files/。不需要任何权限，adb pull 或
// 文件管理器都能取到。想放到别处就用界面上的「导出日志」—— 走系统文件选择器，由用户
// 自己挑位置（那也是 Android 唯一允许应用把文件写到任意位置的途径）。
//
// 只由界面线程写（appendLog 是从主线程调的），所以一把普通锁就够。

package com.adisplay.tv

import android.content.Context
import java.io.BufferedWriter
import java.io.File
import java.io.FileWriter
import java.io.IOException

object LogFile {

    private const val FILE_NAME = "adisplay-log.txt"

    /** 文件上限。超过就从这一行重新开始写 —— 日志是给人看的，不是审计留档，
     *  无上限地涨下去只会让「导出」那一份大到没法用。 */
    private const val MAX_BYTES = 2L * 1024L * 1024L

    private val lock = Any()
    private var file: File? = null
    private var writer: BufferedWriter? = null

    /** 接上（幂等）。由 EngineModel 在 attach 时调用。 */
    fun attach(context: Context) {
        synchronized(lock) {
            if (writer != null) {
                return
            }
            try {
                // 优先外部目录（能 adb pull、也能被文件管理器看到）；拿不到就退到内部目录。
                val dir = context.getExternalFilesDir(null) ?: context.filesDir
                val target = File(dir, FILE_NAME)
                if (target.exists() && target.length() > MAX_BYTES) {
                    FileWriter(target, false).use { it.write("") }
                    // 留一行说明，不然用户看到日志从头开始会以为记录断了。
                    target.appendText("[日志] 文件超过 2 MB，从这里重新开始记录\n")
                }
                file = target
                writer = BufferedWriter(FileWriter(target, true))
            } catch (error: IOException) {
                // 写不了文件不该让程序多一个死法：界面上的日志照旧。
                file = null
                writer = null
            }
        }
    }

    fun append(line: String) {
        synchronized(lock) {
            val out = writer ?: return
            try {
                out.write(line)
                out.newLine()
                // 逐行 flush：真机上崩溃、拔电是常态，而攒在缓冲里的那几行往往正是要看的那几行。
                out.flush()
            } catch (error: IOException) {
                writer = null
            }
        }
    }

    /**
     * 清空文件。
     *
     * 用户点「清除日志」时，**窗口那份与文件这份都要清** —— 只清一份的话，用户以为
     * 已经清干净了，导出时却发现旧内容还在。
     */
    fun clear() {
        synchronized(lock) {
            val target = file ?: return
            try {
                val out = writer
                if (out != null) {
                    out.close()
                }
                FileWriter(target, false).use { it.write("") }
                writer = BufferedWriter(FileWriter(target, true))
            } catch (error: IOException) {
                writer = null
            }
        }
    }

    /** 整份内容，供「导出日志」用。文件不存在或读不了时给空串。 */
    fun read(): String {
        synchronized(lock) {
            val target = file ?: return ""
            return try {
                if (target.exists()) target.readText() else ""
            } catch (error: IOException) {
                ""
            }
        }
    }

    /** 文件路径，给界面显示与日志里记一行用。没有可用路径时给空串。 */
    fun path(): String {
        synchronized(lock) {
            return file?.absolutePath ?: ""
        }
    }
}
