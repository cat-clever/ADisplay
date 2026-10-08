package com.adisplay.tv.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text

/**
 * 待机页底部的日志区。
 *
 * 存在的理由：电视上没有终端、用户也没法 adb logcat，核心库走到哪一步只能
 * 靠它显示出来 —— 这是当前最需要的排障手段（macOS 与 Windows 两端都有同样
 * 的一块）。
 *
 * 两条来自电视端的硬约束：
 *
 *   1. 日志文本绝对不能可聚焦。遥控器的焦点只应该落在按钮上；日志区一旦能
 *      拿到焦点，用户按方向键就跑进日志里，那里既没有可操作的东西，还会把
 *      「确定」键吃掉。所以这里只有 Text，没有任何 focusable / clickable。
 *
 *   2. 新行必须自己出现在可视区底部。既然日志区不吃焦点，就没法用遥控器
 *      滚动它，滚动只能由代码来做。这里用 reverseLayout 的 LazyColumn：
 *      第 0 项摆在底部，整个列表天然锚定在底部，新行推进来不需要任何滚动
 *      调用 —— 也就不会有「等布局量完再滚」那类时序问题。
 */
@Composable
fun LogPanel(
    lines: List<String>,
    textStyle: TextStyle,
    panelHeight: Dp,
    modifier: Modifier = Modifier,
) {
    Box(
        modifier = modifier
            .fillMaxWidth()
            .height(panelHeight)
            .clip(RoundedCornerShape(8.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant)
            // 浅描边只是为了把日志区和待机页的背景分开，别让它看起来是浮在
            // 空中的一段文字。
            .border(1.dp, Color.White.copy(alpha = 0.18f), RoundedCornerShape(8.dp))
            .padding(horizontal = 18.dp, vertical = 12.dp)
    ) {
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
