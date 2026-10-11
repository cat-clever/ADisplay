package com.adisplay.tv.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import android.content.ClipData
import android.content.ClipboardManager
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.tv.material3.Text
import com.adisplay.tv.LogFile

/**
 * 日志内容本身。装在哪由调用方决定 —— 现在只装在抽屉里（见 LogDrawer）。
 *
 * 存在的理由：电视上没有终端、用户也没法 adb logcat，核心库走到哪一步只能
 * 靠它显示出来（macOS 与 Windows 两端都有同样的一份）。
 *
 * 一条来自电视端的硬约束：**日志文本绝对不能可聚焦**。遥控器的焦点只应该落在
 * 按钮上；日志区一旦能拿到焦点，用户按方向键就跑进日志里，那里既没有可操作的
 * 东西，还会把「确定」键吃掉。
 *
 * 也正因为不吃焦点，遥控器滚不动它，滚动只能由代码来做：用 reverseLayout 的
 * LazyColumn，第 0 项摆在底部，整个列表天然锚定在底部，新行推进来不需要任何
 * 滚动调用 —— 也就不会有「等布局量完再滚」那类时序问题。手指可以直接滑（触屏
 * 设备上这是最自然的看法）。
 */
@Composable
fun LogPanel(
    lines: List<String>,
    textStyle: TextStyle,
    modifier: Modifier = Modifier,
) {
    Box(modifier = modifier.fillMaxSize()) {
        LazyColumn(
            reverseLayout = true,
            modifier = Modifier.fillMaxSize()
        ) {
            items(count = lines.size) { index ->
                // reverseLayout 下第 0 项在底部，所以要取倒数的第 index 行。
                Text(
                    text = lines[lines.size - 1 - index],
                    style = textStyle,
                    color = Color.White.copy(alpha = 0.85f),
                    // 限两行：超长的一行（比如媒体地址）不该把整块日志顶成
                    // 一条占满一屏。
                    maxLines = 2
                )
            }
        }
    }
}

/**
 * 日志抽屉：横屏从侧边抽出来，竖屏从下面抽出来（见 Drawer）。
 *
 * 「清除日志」挪进抽屉里，没再占待机页的按钮行 —— 看不见的日志没必要给一个
 * 清除按钮，那一行本来也已经三个按钮了。
 */
@Composable
fun LogDrawer(
    lines: List<String>,
    textStyle: TextStyle,
    buttonTextStyle: TextStyle,
    onClear: () -> Unit,
    onDismiss: () -> Unit,
) {
    val landscape = isLandscape()
    val context = LocalContext.current
    val copyFocus = remember { FocusRequester() }
    val exportFocus = remember { FocusRequester() }
    val clearFocus = remember { FocusRequester() }
    val closeFocus = remember { FocusRequester() }

    // 导出结果那一句话（成功或失败）。放在这里而不是日志里：用户刚点完按钮，
    // 眼睛就在这一块，等翻日志才知道结果太绕。
    var exportNote by remember { mutableStateOf("") }

    // 「导出日志」走系统的文件选择器，由用户自己挑位置 —— 那是 Android 上唯一允许
    // 应用把文件写到应用目录之外的途径（其余地方都要授权）。
    val exportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("text/plain")
    ) { uri ->
        if (uri == null) {
            // 用户在系统界面里按了返回：什么都不做，也不报错 —— 那不是失败。
            return@rememberLauncherForActivityResult
        }
        exportNote = try {
            context.contentResolver.openOutputStream(uri)?.use { stream ->
                stream.write(LogFile.read().toByteArray())
            }
            LocalStrings.current.exportDone
        } catch (error: Exception) {
            LocalStrings.current.exportFailed(error.message ?: error.javaClass.simpleName)
        }
    }

    // 遥控器一进来就有落点：焦点给「关闭」（它在这一块的右上角，方向键往左、往下
    // 能走到三个操作按钮上）。
    LaunchedEffect(Unit) {
        closeFocus.requestFocus()
    }

    Drawer(
        onDismiss = onDismiss,
        modifier = if (landscape) {
            // 横屏从侧边抽：占满高度，宽度按屏幕的一块给 —— 太宽就不像抽屉了。
            Modifier.fillMaxHeight().fillMaxWidth(0.46f)
        } else {
            Modifier.fillMaxWidth().fillMaxHeight(0.55f)
        },
    ) {
        // 两行：第一行标题与「关闭」，第二行三个操作。
        //
        // 四个按钮挤一行放不下 —— 等分也不行，每个只剩 1/4 宽，四个字的标签会被切掉
        // （这一类错这个项目里出现过四次：待机页三个服务按钮、画质那一档、结束投屏横幅、
        // 以及这一行加进第三个按钮那次）。
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(
                text = LocalStrings.current.logLabel,
                style = buttonTextStyle,
                color = Color.White,
            )

            Spacer(modifier = Modifier.weight(1f))

            ActionButton(
                text = LocalStrings.current.close,
                textStyle = buttonTextStyle,
                minWidth = 0.dp,
                focusRequester = closeFocus,
                onClick = onDismiss,
            )
        }

        Spacer(modifier = Modifier.height(10.dp))

        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth(),
        ) {
            ActionButton(
                text = LocalStrings.current.copyAll,
                textStyle = buttonTextStyle,
                minWidth = 0.dp,
                modifier = Modifier.weight(1f),
                focusRequester = copyFocus,
                // 没有日志时压暗：一眼看出这一项此刻没意义（同下面的「清除日志」）。
                enabled = lines.isNotEmpty(),
                onClick = {
                    // 用平台的剪贴板而不是 Compose 那套：后者在 Compose 1.7 起换了 API
                    // （LocalClipboardManager 废弃、新的要挂协程），而这里只是一次同步写入。
                    // 复制的是**整份**日志（界面只显示得下最近这些行），与桌面两端一致。
                    val manager = context.getSystemService(ClipboardManager::class.java)
                    if (manager != null) {
                        manager.setPrimaryClip(
                            ClipData.newPlainText("ADisplay", lines.joinToString("\n"))
                        )
                    }
                },
            )

            Spacer(modifier = Modifier.width(10.dp))

            // 导出成文件：日志可能有几万行，剪贴板不好搬运；而且用户可以自己挑位置。
            // 文件里是**完整**那份（界面上的环只有 500 行，见 LogFile 的文件头）。
            ActionButton(
                text = LocalStrings.current.exportLog,
                textStyle = buttonTextStyle,
                minWidth = 0.dp,
                modifier = Modifier.weight(1f),
                focusRequester = exportFocus,
                onClick = {
                    exportNote = ""
                    exportLauncher.launch("adisplay-log.txt")
                },
            )

            Spacer(modifier = Modifier.width(10.dp))

            ActionButton(
                text = LocalStrings.current.clearLog,
                textStyle = buttonTextStyle,
                minWidth = 0.dp,
                modifier = Modifier.weight(1f),
                focusRequester = clearFocus,
                enabled = lines.isNotEmpty(),
                onClick = onClear,
            )
        }

        if (exportNote.isNotEmpty()) {
            Spacer(modifier = Modifier.height(8.dp))
            Text(text = exportNote, style = textStyle, color = Color.White.copy(alpha = 0.85f))
        }

        Spacer(modifier = Modifier.height(12.dp))

        LogPanel(
            lines = lines,
            textStyle = textStyle,
            modifier = Modifier.weight(1f).fillMaxWidth(),
        )
    }
}
