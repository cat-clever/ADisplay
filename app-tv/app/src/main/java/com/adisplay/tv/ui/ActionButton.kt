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
import androidx.compose.ui.draw.alpha
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
 * 视觉上做三件事：
 *   * 有焦点时加一圈边框并提亮底色 —— 电视上不加的话用户不知道选中了哪个，
 *     按确定键全靠猜。
 *   * 选中态（selected）用另一组底色与边框色，与焦点色区分开 —— 它回答的是
 *     「当前生效的是哪一个」，和「遥控器停在哪」是两回事，选择器上两者要能
 *     同时看出来。
 *   * 触屏上没有焦点概念，边框不出现，看起来就是个普通按钮。
 *
 * enabled = false 用在「核心库没加载起来」这类整块功能都不可用的时候：
 * 按钮留在原地、文字还看得见，只是压暗且按不动 —— 比把按钮藏掉好，
 * 用户看得出是这一块坏了，而不是满屏找一个不存在的按钮。
 */
@Composable
fun ActionButton(
    text: String,
    textStyle: TextStyle,
    minWidth: Dp,
    focusRequester: FocusRequester,
    onClick: () -> Unit,
    enabled: Boolean = true,
    /**
     * 选中态。用在「三选一」这类选择器上（画质档位）：焦点框回答的是「遥控器
     * 现在停在哪」，这个回答的是「当前生效的是哪一个」—— 两者要能同时看出来，
     * 所以底色与边框各用一套颜色。
     */
    selected: Boolean = false,
    /**
     * 紧凑尺寸。用在盖在画面上的悬浮条里 —— 那里是「临时露一下」，按遥控器
     * 能点中的最小范围给就够了，撑成大按钮反而挡画面。
     */
    compact: Boolean = false,
    modifier: Modifier = Modifier,
) {
    var focused by remember { mutableStateOf(false) }

    val accent = MaterialTheme.colorScheme.primary
    val restingBackground = MaterialTheme.colorScheme.surfaceVariant
    val focusedBackground = accent.copy(alpha = 0.35f)

    Box(
        modifier = modifier
            .defaultMinSize(minWidth = minWidth)
            // 压暗整块（底色 + 文字），而不是逐个颜色去算 —— 少一处颜色就少
            // 一处「改了深色主题之后看不见」的风险。
            .alpha(if (enabled) 1f else 0.45f)
            .clip(RoundedCornerShape(10.dp))
            .background(
                when {
                    focused -> focusedBackground
                    selected -> accent.copy(alpha = 0.55f)
                    else -> restingBackground
                }
            )
            // 边框宽度固定，只切颜色。
            // 焦点切换时改宽度会让按钮尺寸跳动，看起来像整块界面在抖。
            //
            // 选中但没聚焦时也要有一圈颜色：用户按完「确定」焦点就移开了，
            // 那时候正是要一眼看出「选的是哪一档」的时候。
            .border(
                width = 2.dp,
                color = when {
                    focused -> Color.White
                    selected -> accent
                    else -> Color.Transparent
                },
                shape = RoundedCornerShape(10.dp),
            )
            // clickable 在前、focusable 在后：触摸点击走前者，
            // 遥控器的方向键走后者，两边都能触发 onClick。
            .clickable(enabled = enabled, onClick = onClick)
            // 禁用时连焦点都不要给：能聚焦却按不动，用户会先怀疑遥控器坏了。
            .focusable(enabled = enabled)
            .focusRequester(focusRequester)
            .onFocusChanged { focused = it.isFocused }
            .padding(
                horizontal = if (compact) 16.dp else 28.dp,
                vertical = if (compact) 8.dp else 14.dp,
            ),
        contentAlignment = Alignment.Center,
    ) {
        Text(text = text, style = textStyle)
    }
}
