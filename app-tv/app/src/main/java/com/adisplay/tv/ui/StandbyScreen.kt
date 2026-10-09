package com.adisplay.tv.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.tv.material3.Surface
import androidx.tv.material3.Text
import com.adisplay.tv.EngineModel

/**
 * 界面入口：按「有没有媒体在播」在待机页与播放页之间切，并负责那两块抽屉。
 *
 * 不另开 Activity —— 文档 2.3 要求投屏开始时自动全屏并置于最上层、结束后回到
 * 待机页，同一个 Activity 里换内容比走 Intent 更直接，也少一层「切回来时
 * 还剩下多少状态」的麻烦。切换的唯一依据是 model.playingMedia：核心说会话
 * 开着就有媒体在播，说关了就没有，界面不自己猜。
 *
 * 抽屉放在这里而不是各自的页面里：日志在待机页与播放页都要能看（投屏中出问题
 * 时正是最该看它的时候），改名的入口只在待机页。抽屉盖在最上层，两个页面共用
 * 同一套开合状态，不用各自记一份。
 */
@Composable
fun StandbyScreen(model: EngineModel) {

    // 日志抽屉开着没有。改名的开合不用另记：草稿在模型里（model.nameDraft），
    // 核心那边要拿它校验名称，界面跟着它走就行。
    var logOpen by remember { mutableStateOf(false) }

    // 抽屉里的字号沿用待机页那套分档参数：两边各写一份，改了一处忘了另一处，
    // 同一块日志在抽屉里和（曾经）页面上的会长得不一样。
    val layout = resolveStandbyLayout(LocalConfiguration.current.screenWidthDp)

    Box(modifier = Modifier.fillMaxSize()) {

        val media = model.playingMedia
        if (model.mirrorSessionId != null) {
            MirrorScreen(
                model = model,
                onExit = { model.stopService() },
                onShowLog = { logOpen = true },
            )
        } else if (media != null) {
            PlaybackScreen(
                model = model,
                sessionId = media.sessionId,
                url = media.url,
                onExit = { model.stopService() },
                onShowLog = { logOpen = true },
            )
        } else {
            StandbyContent(
                model = model,
                onShowLog = { logOpen = true },
            )
        }

        // 抽屉组合在页面之后，所以返回键的处理也排在页面之后 —— 抽屉开着时按
        // 返回先关抽屉，而不是把投屏停掉（见 Drawer）。
        if (logOpen) {
            LogDrawer(
                lines = model.logs,
                textStyle = layout.logStyle,
                buttonTextStyle = layout.actionStyle,
                onClear = { model.clearLogs() },
                onDismiss = { logOpen = false },
            )
        }

        // 改名抽屉。竖屏按内容取高（就三行），横屏占满高度、宽度占一块 ——
        // 横屏从侧边抽，高度不占满的话上面会空出来一大截。
        if (model.nameDraft != null) {
            Drawer(
                onDismiss = { model.cancelRename() },
                modifier = if (isLandscape()) {
                    Modifier.fillMaxHeight().fillMaxWidth(0.42f)
                } else {
                    Modifier.fillMaxWidth()
                },
            ) {
                NameEditor(model = model, layout = layout)
            }
        }
    }
}

/**
 * 待机页。
 *
 * 文档 2.3 的电视端要求：
 *   * 10 英尺界面 —— 大字号，遥控器方向键与「确定 / 返回」键完成全部操作。
 *   * 显示设备名称、局域网 IP 和连接说明，方便手机用户找到设备。
 *
 * 排版参数由 resolveStandbyLayout 按屏幕宽度分档给出，不写死尺寸 ——
 * 电视端的屏幕跨度太大（手机横屏 640dp、1080p 电视 960dp、4K 电视 1920dp），
 * 一套固定字号必然在某一档上不合适。
 *
 * 日志与改名都不在这一页里常驻，收进抽屉（见 StandbyScreen）：常驻会把上面的
 * 按钮往上挤，而这一页的高度是算着放的。
 */
@Composable
private fun StandbyContent(model: EngineModel, onShowLog: () -> Unit) {

    val configuration = LocalConfiguration.current
    val layout = resolveStandbyLayout(configuration.screenWidthDp)
    val inputMode = rememberInputMode()

    val buttonFocus = remember { FocusRequester() }
    val logFocus = remember { FocusRequester() }
    val renameFocus = remember { FocusRequester() }

    Surface(modifier = Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(
                    horizontal = layout.horizontalPadding,
                    vertical = layout.verticalPadding
                ),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            // 内容整体居中，并且可以滚动 —— 矮屏上放不下时裁掉的是最下面那行
            // 提示，而按钮在块的顶部，一定看得见。
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .verticalScroll(rememberScrollState()),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center
            ) {
                Text(
                    text = model.deviceName,
                    style = layout.titleStyle,
                    textAlign = TextAlign.Center
                )

                Spacer(modifier = Modifier.height(layout.spacingTight))

                Text(
                    text = model.statusText,
                    style = layout.statusStyle,
                    textAlign = TextAlign.Center
                )

                // 按钮紧跟状态文字、排在说明之前 —— 矮屏上它必须在首屏可见区内。
                Spacer(modifier = Modifier.height(layout.spacingMedium))

                // 按钮并排而不是上下叠：叠起来会多占一行高度，而矮屏上这个位置
                // 是算着放的。三个按钮用 weight 等分整行宽度 —— 之前用固定宽度，
                // 三个加起来超过了屏宽，第三个（修改名称）被挤出可视区。
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.Center,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    ActionButton(
                        text = if (model.serviceEnabled) "关闭接收服务" else "开启接收服务",
                        textStyle = layout.actionStyle,
                        minWidth = 0.dp,
                        modifier = Modifier.weight(1f),
                        focusRequester = buttonFocus,
                        // 核心库没加载起来时按钮压暗且不可聚焦：让用户看出是这一块
                        // 坏了，而不是对着一个按了没反应的按钮猜网络。
                        enabled = model.isNativeAvailable,
                        onClick = { model.toggleService() },
                    )

                    Spacer(modifier = Modifier.width(16.dp))

                    // 日志收进抽屉（横屏从侧边抽、竖屏从下面抽），「清除日志」
                    // 也跟着挪进抽屉里 —— 看不见的日志不需要一个常驻的清除按钮。
                    ActionButton(
                        text = "查看日志",
                        textStyle = layout.actionStyle,
                        minWidth = 0.dp,
                        modifier = Modifier.weight(1f),
                        focusRequester = logFocus,
                        onClick = onShowLog,
                    )

                    Spacer(modifier = Modifier.width(16.dp))

                    // 电视上没有键盘，改名的入口必须显式给出来。名字是手机端
                    // 投屏列表里看到的东西，改不了就只能认设备型号那一个默认名。
                    ActionButton(
                        text = "修改名称",
                        textStyle = layout.actionStyle,
                        minWidth = 0.dp,
                        modifier = Modifier.weight(1f),
                        focusRequester = renameFocus,
                        onClick = { model.beginRename() },
                    )
                }

                Spacer(modifier = Modifier.height(layout.spacingLoose))

                // 连接说明。用户的典型困惑是「手机搜不到电视」，多半是没连同一个
                // Wi-Fi 或路由器开了 AP 隔离（文档 6.3），所以把提示写在待机页上。
                //
                // widthIn 限制最大宽度：4K 电视上如果让文字铺满 1920dp，
                // 每行会太长而不便阅读。
                // 提示文案按输入方式区分：用户拿遥控器时提醒他按确定键，
                // 拿手机时提醒他点击。写反了会让人对着屏幕按半天。
                val hintLead = if (inputMode == InputMode.Remote) {
                    "用遥控器选中上方按钮并按「确定」开启接收服务。\n"
                } else {
                    "点击上方按钮开启接收服务。\n"
                }

                Text(
                    text = hintLead +
                        "请让手机与本机连接同一个 Wi-Fi，然后：\n" +
                        "iPhone：控制中心 → 屏幕镜像\n" +
                        "Android：视频 App 里的「投屏」按钮",
                    style = layout.bodyStyle,
                    textAlign = TextAlign.Center,
                    modifier = Modifier
                        .fillMaxWidth()
                        .widthIn(max = 900.dp)
                )

                // 本机地址：手机搜不到设备时，DLNA 类 App 可以直接手填这个地址，
                // 也是判断「电视到底在哪张网卡上」最快的办法（文档 2.3 要求显示）。
                if (model.localAddresses.isNotEmpty()) {
                    Spacer(modifier = Modifier.height(layout.spacingTight))

                    Text(
                        text = "本机地址：" + model.localAddresses.replace("\n", "  "),
                        style = layout.bodyStyle,
                        textAlign = TextAlign.Center,
                        modifier = Modifier.fillMaxWidth()
                    )
                }
            }
        }
    }

    // 只有遥控器设备才自动抢焦点。
    //
    // 电视上不这么做的话，用户按方向键没有任何反馈，会以为应用卡死；
    // 但触屏设备上抢焦点会让焦点框一直挂在按钮上，看起来像没点干净。
    LaunchedEffect(inputMode) {
        if (inputMode == InputMode.Remote) {
            buttonFocus.requestFocus()
        }
    }
}
