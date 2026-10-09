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
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.tv.material3.Text

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
    val clearFocus = remember { FocusRequester() }
    val closeFocus = remember { FocusRequester() }

    // 遥控器一进来就有落点：焦点给「关闭」，方向键能走到「清除日志」。
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
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(
                text = "日志",
                style = buttonTextStyle,
                color = Color.White,
            )

            Spacer(modifier = Modifier.weight(1f))

            ActionButton(
                text = "清除日志",
                textStyle = buttonTextStyle,
                minWidth = 0.dp,
                focusRequester = clearFocus,
                // 没有日志时压暗：一眼看出这一项此刻没意义。
                enabled = lines.isNotEmpty(),
                onClick = onClear,
            )

            Spacer(modifier = Modifier.width(12.dp))

            ActionButton(
                text = "关闭",
                textStyle = buttonTextStyle,
                minWidth = 0.dp,
                focusRequester = closeFocus,
                onClick = onDismiss,
            )
        }

        Spacer(modifier = Modifier.height(12.dp))

        LogPanel(
            lines = lines,
            textStyle = textStyle,
            modifier = Modifier.weight(1f).fillMaxWidth(),
        )
    }
}
