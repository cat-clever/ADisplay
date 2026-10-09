package com.adisplay.tv.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
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
import androidx.compose.runtime.remember
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
 * 界面入口：按「有没有媒体在播」在待机页与播放页之间切。
 *
 * 不另开 Activity —— 文档 2.3 要求投屏开始时自动全屏并置于最上层、结束后回到
 * 待机页，同一个 Activity 里换内容比走 Intent 更直接，也少一层「切回来时
 * 还剩下多少状态」的麻烦。切换的唯一依据是 model.playingMedia：核心说会话
 * 开着就有媒体在播，说关了就没有，界面不自己猜。
 */
@Composable
fun StandbyScreen(model: EngineModel) {
    // 镜像与「媒体地址」是两条不同的路：前者是持续的帧，后者是一条 URL。
    // 镜像优先 —— 同一时刻只可能有一条在跑，但先判它更符合因果（镜像会话
    // 建立时不会有 playingMedia）。
    if (model.mirrorSessionId != null) {
        MirrorScreen(
            model = model,
            onExit = { model.stopService() },
        )
        return
    }

    val media = model.playingMedia
    if (media != null) {
        PlaybackScreen(
            model = model,
            sessionId = media.sessionId,
            url = media.url,
            // 「停止接收投屏」停的是整个接收服务：JNI 面上没有「只关掉某一个
            // 会话」的接口（见 AdDisplayNative），停服务会让核心把会话关掉、
            // 状态回到待机页 —— 这条路走完界面和核心是一致的，不会留下一个
            // 核心还当活着、界面已经不管了的会话。用户想再投一次，按一下
            // 「开启接收服务」重新进入广播即可。
            onExit = { model.stopService() },
        )
    } else {
        StandbyContent(model)
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
 * 下半屏是日志区（LogPanel）。它不是装饰：电视上没法看 logcat，核心库
 * 走到哪一步、SSDP 有没有收到搜索、端口有没有被占用，全凭它显示。
 */
@Composable
private fun StandbyContent(model: EngineModel) {

    val configuration = LocalConfiguration.current
    val layout = resolveStandbyLayout(configuration.screenWidthDp)
    val inputMode = rememberInputMode()

    val buttonFocus = remember { FocusRequester() }
    val clearLogFocus = remember { FocusRequester() }
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
            // 上半屏：设备信息与开关。
            //
            // 用 weight 吃掉日志区之外的全部高度，并且自己可以滚动 ——
            // 矮屏上内容放不下时裁掉的是最下面那行提示，而按钮在块的顶部，
            // 一定看得见。（日志区不能跟着被裁：没有焦点，用户滚不到它。）
            Column(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth()
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

                // 两个按钮并排而不是上下叠：叠起来会多占一行高度，而矮屏上
                // 这个位置是算着放的（见上面的注释），日志区会被挤掉。
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.Center,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    ActionButton(
                        text = if (model.serviceEnabled) "关闭接收服务" else "开启接收服务",
                        textStyle = layout.actionStyle,
                        minWidth = layout.buttonMinWidth,
                        focusRequester = buttonFocus,
                        // 核心库没加载起来时按钮压暗且不可聚焦：让用户看出是这一块
                        // 坏了，而不是对着一个按了没反应的按钮猜网络。
                        enabled = model.isNativeAvailable,
                        onClick = { model.toggleService() },
                    )

                    Spacer(modifier = Modifier.width(16.dp))

                    ActionButton(
                        text = "清除日志",
                        textStyle = layout.actionStyle,
                        minWidth = layout.buttonMinWidth,
                        focusRequester = clearLogFocus,
                        // 没有日志时压暗：一眼看出这一项此刻没意义。
                        enabled = model.logs.isNotEmpty(),
                        onClick = { model.clearLogs() },
                    )

                    Spacer(modifier = Modifier.width(16.dp))

                    // 电视上没有键盘，改名的入口必须显式给出来。名字是手机端
                    // 投屏列表里看到的东西，改不了就只能认设备型号那一个默认名。
                    ActionButton(
                        text = "修改名称",
                        textStyle = layout.actionStyle,
                        minWidth = layout.buttonMinWidth,
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

            Spacer(modifier = Modifier.height(layout.spacingLoose))

            // 底部这块地方：平时是日志，改名时借用它做输入区。
            //
            // 借用的理由：电视的布局是算着屏幕高度放的（见 StandbyLayout），
            // 在页面里另加一块会把上面的按钮挤出可视区；而这块高度固定，
            // 正好换得下。
            if (model.nameDraft != null) {
                NameEditor(model = model, layout = layout)
            } else {
                // 日志区钉在底部，高度固定。它不可聚焦，所以遥控器的焦点
                // 始终留在上面的按钮上。
                LogPanel(
                    lines = model.logs,
                    textStyle = layout.logStyle,
                    panelHeight = layout.logHeight
                )
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
