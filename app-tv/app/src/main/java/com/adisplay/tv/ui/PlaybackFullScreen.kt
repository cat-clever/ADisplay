// ADisplay —— 播放期间的「全屏 + 悬浮控件」
//
// 投屏时用户看的就是画面，所以一进播放页就把状态栏和导航栏都藏掉，画面铺到
// 屏幕边缘；离开时再还回去 —— 一直藏着的话，待机页那行设备名会被状态栏压住，
// 用户退到桌面后系统栏也不见了。
//
// 控件（顶部状态条、底部「停止接收投屏」）是**浮在画面上**的，不占画面的高度：
// 独占一行会把画面压扁一块，而这块区域本来该全是画面。控件默认收起，点一下
// 画面、或者遥控器按任意键才出现，几秒后自动收起（见 CONTROLS_TIMEOUT_MS）。
//
// 退路：
//   * 控件出现后按「停止接收投屏」（触屏点它、遥控器选中它按确定）
//   * 遥控器「返回」键
//   * 遥控器「菜单」键（在 MainActivity.onKeyDown 里处理）
// 给够几条是因为电视只有遥控器 —— 一旦卡在播放页又没有任何可见的按钮，
// 用户唯一的办法是拔电源。

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
import androidx.compose.foundation.focusable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.dp
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text
import kotlinx.coroutines.delay

/** 控件出现后停留多久自动收起。 */
private const val CONTROLS_TIMEOUT_MS = 4000L

/**
 * 全屏播放画面，控件悬浮其上。
 *
 * @param title 顶部状态条左侧的文字，调用方给（一般是「设备名 · 正在做什么」）。
 * @param onShowLog 打开日志抽屉。日志在投屏中才最该看，所以这里也留一个入口。
 * @param captureTouches 画面自己处理触摸时传 false（DLNA 那路用 PlayerView 自带的
 *   控制条：进度、快进快退）。我们绝不能在它上面再盖一层接触摸的东西 —— 那样点击
 *   会被我们抢走，播放器的控制条永远出不来，而**拖动进度条必须让触摸落到播放器
 *   上**。镜像那路画面是个纯 Surface，不处理触摸，所以默认 true（点画面唤出控件）。
 * @param showToken 外部信号：每递增一次就把控件亮出来一遍。播放器把控制条亮出来时
 *   由它递增，于是「点画面」一次就让播放器的进度条和我们的悬浮条一起出现。
 * @param content 画面本身：镜像是一条 Surface，DLNA 是一个 PlayerView。
 */
@Composable
fun PlaybackFullScreen(
    title: String,
    onExit: () -> Unit,
    onShowLog: () -> Unit,
    captureTouches: Boolean = true,
    showToken: Int = 0,
    content: @Composable BoxScope.() -> Unit,
) {
    val view = LocalView.current
    val layout = resolveStandbyLayout(LocalConfiguration.current.screenWidthDp)

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

    // 遥控器「返回」键走 Activity 的返回分派器，不受焦点影响。
    BackHandler { onExit() }

    // 用「请求计数」而不是一个 Boolean：控件已经显示时再点一下，也要把倒计时
    // 重新计起（用户正在看控件，不该正好在这一刻收走）。Boolean 从 true 再赋
    // true 不产生状态变化，LaunchedEffect 不会重启，计时也就不会重置。
    var showRequest by remember { mutableStateOf(0) }
    var controlsVisible by remember { mutableStateOf(false) }

    LaunchedEffect(showRequest) {
        if (showRequest == 0) {
            return@LaunchedEffect
        }
        controlsVisible = true
        delay(CONTROLS_TIMEOUT_MS)
        controlsVisible = false
    }

    // 画面那边把控制条亮出来了（见 showToken）。跟着亮一次。
    LaunchedEffect(showToken) {
        if (showToken > 0) {
            showRequest += 1
        }
    }

    val catchFocus = remember { FocusRequester() }
    val logFocus = remember { FocusRequester() }
    val stopFocus = remember { FocusRequester() }

    // 收起时把焦点交给「接键层」，遥控器按任意键都能把控件叫回来；
    // 显示时交给「停止接收投屏」，用户按确定就能退出。
    LaunchedEffect(controlsVisible) {
        if (controlsVisible) {
            stopFocus.requestFocus()
        } else {
            catchFocus.requestFocus()
        }
    }

    Box(modifier = Modifier.fillMaxSize().background(Color.Black)) {
        content()

        // 触摸层。只在画面自己不处理触摸时铺（镜像那条）。
        //
        // 交互源与 indication 都留空：这是「点哪儿都显示控件」，不该闪波纹。
        if (captureTouches) {
            Box(
                modifier = Modifier
                    .matchParentSize()
                    .clickable(
                        interactionSource = remember { MutableInteractionSource() },
                        indication = null,
                        onClick = { showRequest++ },
                    )
            )
        }

        // 接键层：只接遥控器的按键，**一点触摸面积都不占**。
        //
        // 这一层以前是铺满屏幕的，于是把点击从播放器手里抢走了 —— DLNA 那路的
        // 进度条与快进快退就此永远出不来。焦点不需要面积：1dp 的节点一样能被
        // focusRequester 选中、一样收得到按键。
        Box(
            modifier = Modifier
                .align(Alignment.Center)
                .size(1.dp)
                .focusRequester(catchFocus)
                .focusable()
                .onKeyEvent { event ->
                    // 只在抬起时响应，否则按下与抬起各触发一次。
                    if (event.type != KeyEventType.KeyUp) {
                        return@onKeyEvent false
                    }
                    // 「返回」与「菜单」必须放过去。
                    //
                    // 按键先走视图树、没被消费才轮到 Activity 的 onKeyDown，
                    // 所以这两个键一旦在这里吃掉，「返回」就到不了返回分派器
                    // （BackHandler 不再触发），「菜单」也到不了
                    // MainActivity.onKeyDown —— 两条退路会一起失效。
                    if (event.key == Key.Back || event.key == Key.Menu) {
                        return@onKeyEvent false
                    }
                    showRequest++
                    true
                }
        )

        // 顶部状态条。悬浮，不占画面高度。
        AnimatedVisibility(
            visible = controlsVisible,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier.align(Alignment.TopCenter),
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Color.Black.copy(alpha = 0.55f))
                    .padding(horizontal = 24.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleMedium,
                    color = Color.White,
                )

                Spacer(modifier = Modifier.weight(1f))

                Text(
                    text = "点画面可再显示控件，按「返回」结束投屏",
                    style = layout.bodyStyle,
                    color = Color.White.copy(alpha = 0.75f),
                )
            }
        }

        // 底部操作条。同样悬浮。
        AnimatedVisibility(
            visible = controlsVisible,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier.align(Alignment.BottomCenter),
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    // 播放器的控制条（进度、快进快退）就在屏幕最下面，我们这条
                    // 往上让开它 —— 两条叠在一起既看不清也点不准。
                    .padding(bottom = if (captureTouches) 0.dp else 104.dp)
                    .background(Color.Black.copy(alpha = 0.55f))
                    .padding(horizontal = 24.dp, vertical = 14.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.Center,
            ) {
                ActionButton(
                    text = "日志",
                    textStyle = layout.actionStyle,
                    minWidth = layout.buttonMinWidth,
                    focusRequester = logFocus,
                    onClick = onShowLog,
                )

                Spacer(modifier = Modifier.width(16.dp))

                ActionButton(
                    text = "停止接收投屏",
                    textStyle = layout.actionStyle,
                    minWidth = layout.buttonMinWidth,
                    focusRequester = stopFocus,
                    onClick = onExit,
                )
            }
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
