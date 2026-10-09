// ADisplay —— 抽屉面板
//
// 日志与改名都装在这里面。为什么做成抽屉而不是待机页上常驻一块：常驻会把上面
// 的按钮往上挤，而电视端的布局是算着屏幕高度放的（见 StandbyLayout）。抽屉盖在
// 内容之上，不参与那套计算，谁也不挤谁。
//
// 方向跟着屏幕朝向走：
//   竖屏  从下往上抽
//   横屏  从侧边抽
// 竖屏的宽度就那么点，从侧边抽出来太窄、句子全折行；横屏的高度就那么点，
// 从下面抽出来会把画面压扁。
//
// 面板与幕布都带透明度：用户仍看得见底下的待机页，知道自己没有离开这一页 ——
// 抽屉是「临时看一眼」，不是跳转。

package com.adisplay.tv.ui

import android.content.res.Configuration
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideInVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.unit.dp
import androidx.tv.material3.MaterialTheme

/** 屏幕是不是横着的。抽屉方向由它决定。 */
@Composable
fun isLandscape(): Boolean {
    return LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
}

/**
 * 一块贴着屏幕边缘抽出来的面板。
 *
 * @param modifier 面板尺寸由调用方给：日志要占满一侧，改名按内容取高。
 * @param content 面板里的内容。
 */
@Composable
fun Drawer(
    onDismiss: () -> Unit,
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit,
) {
    val landscape = isLandscape()

    // 进场动画：先以「收起」状态组合一次，再翻成展开 —— AnimatedVisibility 的
    // 进入动画就是这么触发的（一上来就 visible=true 的话它直接出现，不动）。
    var entered by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        entered = true
    }

    // 抽屉开着时，返回键先关抽屉。
    //
    // 它注册在页面之后，所以比页面的返回处理优先 —— 否则在投屏中打开日志，
    // 一按返回就把投屏停掉了，用户想关的只是抽屉。
    BackHandler { onDismiss() }

    Box(modifier = Modifier.fillMaxSize()) {
        // 幕布：半透明黑，点它关闭。
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(Color.Black.copy(alpha = 0.45f))
                .clickable(
                    interactionSource = remember { MutableInteractionSource() },
                    indication = null,
                    onClick = onDismiss,
                )
        )

        // 圆角只留靠内的一侧：贴边那侧做成圆角会在屏幕边缘露出一道缝。
        val shape = if (landscape) {
            RoundedCornerShape(topStart = 14.dp, bottomStart = 14.dp)
        } else {
            RoundedCornerShape(topStart = 14.dp, topEnd = 14.dp)
        }

        AnimatedVisibility(
            visible = entered,
            enter = if (landscape) {
                slideInHorizontally { width -> width }
            } else {
                slideInVertically { height -> height }
            },
            modifier = if (landscape) {
                Modifier.align(Alignment.CenterEnd)
            } else {
                Modifier.align(Alignment.BottomCenter)
            },
        ) {
            Column(
                modifier = modifier
                    .clip(shape)
                    // 面板本身也带透明度：底下的待机页透出来一点，一眼看得出
                    // 它是浮在上面的一层，而不是换了一页。
                    .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.94f))
                    .padding(horizontal = 20.dp, vertical = 16.dp),
                content = content,
            )
        }
    }
}
