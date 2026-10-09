// ADisplay —— 播放期间的「全屏 + 退路」
//
// 投屏时用户看的就是画面，所以一进播放页就把状态栏和导航栏都藏掉，画面铺到
// 屏幕边缘；离开时再还回去 —— 一直藏着的话，待机页那行设备名会被状态栏压住，
// 用户退到桌面后系统栏也不见了。
//
// 退路给三条，触屏和遥控器各有顺手的：
//   * 点画面（触屏）
//   * 遥控器「返回」键
//   * 遥控器「菜单」键
// 三条做同一件事：结束本次投屏、回待机页。给够三条是因为电视只有遥控器 ——
// 一旦卡在播放页又没有任何可见的按钮，用户唯一的办法是拔电源。

package com.adisplay.tv.ui

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text
import kotlinx.coroutines.delay

/** 退出提示停留的时长。 */
private const val HINT_DURATION_MS = 3000L

/**
 * 把画面铺满屏幕，并接好三条退路。
 *
 * 画面本身当 content 传进来（镜像是一条 Surface、DLNA 是一个 PlayerView），
 * 这里不关心它是什么。
 */
@Composable
fun PlaybackFullScreen(onExit: () -> Unit, content: @Composable BoxScope.() -> Unit) {
    val view = LocalView.current

    DisposableEffect(view) {
        val window = view.context.findActivity()?.window
        val controller = if (window != null) {
            WindowCompat.getInsetsController(window, view)
        } else {
            null
        }
        // 划一下可以临时把系统栏叫回来。电视上用不着，手机上很自然。
        controller?.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        controller?.hide(WindowInsetsCompat.Type.systemBars())

        onDispose {
            controller?.show(WindowInsetsCompat.Type.systemBars())
        }
    }

    // 遥控器「返回」键走 Activity 的返回分派器，不依赖焦点落在哪儿。
    // 遥控器「菜单」键在 MainActivity.onKeyDown 里处理：Compose 的按键事件只送到
    // 当前聚焦的元素，而为了全屏，播放页里没有放任何可聚焦的东西。
    BackHandler { onExit() }

    var hintVisible by remember { mutableStateOf(true) }
    LaunchedEffect(Unit) {
        delay(HINT_DURATION_MS)
        hintVisible = false
    }

    Box(modifier = Modifier.fillMaxSize().background(Color.Black)) {
        content()

        // 盖在画面上的透明触摸层。
        //
        // 必须有这一层，不能只靠外层的 clickable：DLNA 播放页里的 PlayerView
        // 自己消费触摸（它要用触摸开关控制条），事件传不到父节点。这一层声明在
        // content 之后，命中顺序在最上面，点哪儿都落到这里。
        //
        // 交互源与 indication 都留空：这是「点哪儿都退出」，不该在画面上闪波纹。
        Box(
            modifier = Modifier
                .matchParentSize()
                .clickable(
                    interactionSource = remember { MutableInteractionSource() },
                    indication = null,
                    onClick = onExit,
                )
        )

        // 提示只留几秒。全屏是用户要的，常驻一行字等于没全屏；但也完全不说不行
        // —— 触屏上没有任何按钮，不说用户不知道怎么退出去。
        AnimatedVisibility(
            visible = hintVisible,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier.align(Alignment.BottomCenter),
        ) {
            Text(
                text = "点画面，或按遥控器的「返回」「菜单」键，结束本次投屏",
                style = MaterialTheme.typography.bodyMedium,
                color = Color.White.copy(alpha = 0.85f),
                textAlign = TextAlign.Center,
                modifier = Modifier
                    .padding(bottom = 40.dp)
                    .clip(RoundedCornerShape(8.dp))
                    .background(Color.Black.copy(alpha = 0.55f))
                    .fillMaxWidth()
                    .padding(horizontal = 24.dp, vertical = 12.dp),
            )
        }
    }
}

/** Compose 的 view 挂在一层层 ContextWrapper 下，一路往外找到 Activity。 */
private fun Context.findActivity(): Activity? {
    var current: Context? = this
    while (current is ContextWrapper) {
        if (current is Activity) {
            return current
        }
        current = current.baseContext
    }
    return null
}
