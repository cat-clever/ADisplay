package com.adisplay.tv.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text

/**
 * 一个同时认触摸与遥控器的按钮。
 *
 * 为什么不用 androidx.tv.material3.Button：那个组件是为电视设计的，
 * 交互模型建立在「焦点 + 方向键」上，在触屏设备上点击不生效 ——
 * 表现就是「按钮看得见，点上去没反应」。而项目要求同一个 APK 既要跑在
 * 电视上（只有遥控器）又要跑在手机/平板上（触屏）。
 *
 * 所以这里用 Compose 的基础组件自己拼一个：
 *   clickable   处理触摸点击
 *   focusable   处理遥控器方向键导航
 * 两者可以共存，互不干扰。
 *
 * 视觉上做两件事：
 *   * 有焦点时加一圈边框并提亮底色 —— 电视上不加的话用户不知道选中了哪个，
 *     按确定键全靠猜。
 *   * 触屏上没有焦点概念，边框不出现，看起来就是个普通按钮。
 */
@Composable
fun ActionButton(
    text: String,
    textStyle: TextStyle,
    minWidth: Dp,
    focusRequester: FocusRequester,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    var focused by remember { mutableStateOf(false) }

    val accent = MaterialTheme.colorScheme.primary
    val restingBackground = MaterialTheme.colorScheme.surfaceVariant
    val focusedBackground = accent.copy(alpha = 0.35f)

    Box(
        modifier = modifier
            .defaultMinSize(minWidth = minWidth)
            .clip(RoundedCornerShape(10.dp))
            .background(if (focused) focusedBackground else restingBackground)
            // 边框宽度固定，只切颜色。
            // 焦点切换时改宽度会让按钮尺寸跳动，看起来像整块界面在抖。
            .border(
                width = 2.dp,
                color = if (focused) Color.White else Color.Transparent,
                shape = RoundedCornerShape(10.dp),
            )
            // clickable 在前、focusable 在后：触摸点击走前者，
            // 遥控器的方向键走后者，两边都能触发 onClick。
            .clickable(onClick = onClick)
            .focusable()
            .focusRequester(focusRequester)
            .onFocusChanged { focused = it.isFocused }
            .padding(horizontal = 28.dp, vertical = 14.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(text = text, style = textStyle)
    }
}
