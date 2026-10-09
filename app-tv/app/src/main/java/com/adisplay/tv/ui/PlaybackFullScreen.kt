// ADisplay —— 播放期间的「全屏 + 悬浮控件」
//
// 投屏时用户看的就是画面，所以一进播放页就把状态栏和导航栏都藏掉，画面铺到
// 屏幕边缘；离开时再还回去 —— 一直藏着的话，待机页那行设备名会被状态栏压住，
// 用户退到桌面后系统栏也不见了。
//
// 控件（顶部状态条、右侧那列「结束投屏」等按钮）是**浮在画面上**的，不占画面高度：
// 独占一行会把画面压扁一块，而这块区域本来该全是画面。控件默认收起，点一下
// 画面、或者遥控器按任意键才出现，几秒后自动收起（见 CONTROLS_TIMEOUT_MS）。
//
// 退路：
//   * 控件出现后按「结束投屏」（触屏点它、遥控器选中它按确定）
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
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
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

/** 一次快进/快退跨多少毫秒。 */
private const val SEEK_STEP_MS = 10_000L

/**
 * 悬浮条上的传输控制。
 *
 * 为什么要有它：焦点只能有一个主人。遥控器的按键落到我们这条上，播放器自带
 * 的控制条就够不到了（它的控制条要靠 PlayerView 自己拿到按键才弹得出来）——
 * 遥控器用户于是按不了暂停、快进快退。所以遥控器那条路得由我们这条把传输
 * 也管起来（文档 2.3 要的是「遥控器完成全部操作」）。
 *
 * 触屏那条路不用它：点击直接落到播放器上，它自带的控制条照常出现。
 */
class PlaybackTransport(
    val isPlaying: Boolean,
    val onTogglePlay: () -> Unit,
    val onSeekBy: (Long) -> Unit,
)

/**
 * 全屏播放画面，控件悬浮其上。
 *
 * @param title 顶部状态条左侧的文字，调用方给（一般是「设备名 · 正在做什么」）。
 * @param onShowLog 打开日志抽屉。日志在投屏中才最该看，所以这里也留一个入口。
 * @param captureTouches 画面自己处理触摸时传 false（DLNA 那路用 PlayerView 自带的
 *   控制条：进度、快进快退）。我们绝不能在它上面再盖一层接触摸的东西 —— 那样点击
 *   会被我们抢走，播放器的控制条永远出不来，而**拖动进度条必须让触摸落到播放器
 *   上**。镜像那路画面是个纯 Surface，不处理触摸，所以默认 true（点画面唤出控件）。
 * @param playerControlsVisible 画面自己的控制条现在可不可见。给了它（DLNA 那路）
 *   就以它为准：播放器的进度条一出来我们这条跟着出来、它一收我们跟着收 ——
 *   两条各用各的计时器的话，用户会看到「进度条还在、日志按钮却没了」这种错位。
 * @param transport 传输控制（暂停、快进、快退）。遥控器那条路必须给，否则遥控器
 *   用户碰不到播放器的控制条；触屏那条路给 null 就行（播放器自己带）。
 * @param content 画面本身：镜像是一条 Surface，DLNA 是一个 PlayerView。
 */
@Composable
fun PlaybackFullScreen(
    title: String,
    onExit: () -> Unit,
    onShowLog: () -> Unit,
    captureTouches: Boolean = true,
    playerControlsVisible: Boolean? = null,
    transport: PlaybackTransport? = null,
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

    // 自己那套计时（点画面 / 遥控器唤出）。镜像那条走它 —— 那路没有播放器控制条。
    var localVisible by remember { mutableStateOf(false) }

    LaunchedEffect(showRequest) {
        if (showRequest == 0) {
            return@LaunchedEffect
        }
        localVisible = true
        delay(CONTROLS_TIMEOUT_MS)
        localVisible = false
    }

    // 两条信号取「或」：
    //   自己唤出的（遥控器那条路）—— 会有 4 秒后自动收起；
    //   画面自己的控制条可见 —— 由它说了算，它收我们跟着收。
    // 这样点画面那一次，进度条和日志按钮是同时出现、同时消失的。
    val controlsVisible = localVisible || playerControlsVisible == true

    val catchFocus = remember { FocusRequester() }
    val backFocus = remember { FocusRequester() }
    val playFocus = remember { FocusRequester() }
    val forwardFocus = remember { FocusRequester() }
    val logFocus = remember { FocusRequester() }
    val stopFocus = remember { FocusRequester() }

    // 收起时把焦点交给「接键层」，遥控器按任意键都能把控件叫回来；
    // 显示时交给「结束投屏」，用户按确定就能退出。
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
                // 控件亮着的时候把自己从焦点搜索里摘掉。
                //
                // 它是一个焦点节点，又正好杵在两个按钮中间 —— 不摘掉的话，
                // 遥控器按方向键很可能选中它（然后按确定只会再「显示一次控件」），
                // 用户就换不到旁边那两个按钮上。
                .focusable(enabled = !controlsVisible)
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

        // 操作按钮：贴在右侧竖排。
        //
        // 不排底部：播放器自己的进度条与快进快退就在屏幕最下面，两条挤在一起
        // 既看不清也点不准。竖排还有个好处 —— 拇指 / 遥控器上下走一遍就能全过。
        AnimatedVisibility(
            visible = controlsVisible,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier.align(Alignment.CenterEnd),
        ) {
            Column(
                modifier = Modifier.padding(end = 24.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
                horizontalAlignment = Alignment.End,
            ) {
                // 传输控制。只在遥控器那条路上给（见 PlaybackTransport 的说明）。
                if (transport != null) {
                    ActionButton(
                        text = "-10秒",
                        textStyle = layout.actionStyle,
                        minWidth = 0.dp,
                        focusRequester = backFocus,
                        compact = true,
                        onClick = { transport.onSeekBy(-SEEK_STEP_MS) },
                    )

                    ActionButton(
                        text = if (transport.isPlaying) "暂停" else "播放",
                        textStyle = layout.actionStyle,
                        minWidth = 0.dp,
                        focusRequester = playFocus,
                        compact = true,
                        onClick = transport.onTogglePlay,
                    )


                    ActionButton(
                        text = "+10秒",
                        textStyle = layout.actionStyle,
                        minWidth = 0.dp,
                        focusRequester = forwardFocus,
                        compact = true,
                        onClick = { transport.onSeekBy(SEEK_STEP_MS) },
                    )

                }

                // 紧凑尺寸：这条是盖在画面上的，按最小可点范围给就够了。
                ActionButton(
                    text = "日志",
                    textStyle = layout.actionStyle,
                    minWidth = 0.dp,
                    focusRequester = logFocus,
                    compact = true,
                    onClick = onShowLog,
                )


                ActionButton(
                    // 只能说「结束投屏」：它已经不停接收服务了（见 EngineModel
                    // 的 dismissedEpisode）—— 服务一停手机那边就找不到这台设备。
                    text = "结束投屏",
                    textStyle = layout.actionStyle,
                    minWidth = 0.dp,
                    focusRequester = stopFocus,
                    compact = true,
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
