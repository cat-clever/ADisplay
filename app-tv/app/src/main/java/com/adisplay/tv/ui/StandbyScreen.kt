package com.adisplay.tv.ui

import android.os.Build
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
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
import androidx.tv.material3.Button
import androidx.tv.material3.Surface
import androidx.tv.material3.Text

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
 * 批次 5 之前这个界面只是骨架：按钮能切换文案，但不驱动真实接收服务
 * （那要等 JNI 把 castcore 接进来）。
 */
@Composable
fun StandbyScreen() {

    val configuration = LocalConfiguration.current
    val layout = resolveStandbyLayout(configuration.screenWidthDp)
    val inputMode = rememberInputMode()

    // 电视端默认设备名取设备型号（文档 2.4）。
    val deviceName = remember { Build.MODEL.ifBlank { "Android TV" } }
    var serviceEnabled by remember { mutableStateOf(false) }

    val buttonFocus = remember { FocusRequester() }

    Surface(modifier = Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                // 极矮的屏上内容仍可能超出，允许滚动查看。
                .verticalScroll(rememberScrollState())
                .padding(
                    horizontal = layout.horizontalPadding,
                    vertical = layout.verticalPadding
                ),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            Text(
                text = deviceName,
                style = layout.titleStyle,
                textAlign = TextAlign.Center
            )

            Spacer(modifier = Modifier.height(layout.spacingTight))

            Text(
                text = if (serviceEnabled) "等待手机连接…" else "接收服务已关闭",
                style = layout.statusStyle,
                textAlign = TextAlign.Center
            )

            // 按钮紧跟状态文字、排在说明之前 —— 矮屏上它必须在首屏可见区内。
            Spacer(modifier = Modifier.height(layout.spacingMedium))

            Button(
                onClick = { serviceEnabled = !serviceEnabled },
                modifier = Modifier
                    .defaultMinSize(minWidth = layout.buttonMinWidth)
                    .focusRequester(buttonFocus)
            ) {
                Text(
                    text = if (serviceEnabled) "关闭接收服务" else "开启接收服务",
                    style = layout.actionStyle
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
