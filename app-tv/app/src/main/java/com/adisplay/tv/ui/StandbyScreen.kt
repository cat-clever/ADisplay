package com.adisplay.tv.ui

import android.os.Build
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.tv.material3.Button
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.runtime.LaunchedEffect
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Surface
import androidx.tv.material3.Text

/**
 * 待机页。
 *
 * 文档 2.3 的要求：
 *   * 10 英尺界面 —— 大字号，遥控器方向键与「确定 / 返回」键完成全部操作。
 *   * 显示设备名称、局域网 IP 和连接说明（含二维码），方便手机用户找到设备。
 *
 * 布局上踩过一次坑：清单里锁了横屏，而横屏下可用高度只有 360dp 左右
 * （手机横屏）到 540dp（1080p 电视）。原来用 Arrangement.Center 且不可滚动，
 * 内容一超高就从上下均等裁掉，底部的按钮正好被切走 —— 表现是「看不到
 * 任何可点的东西」。所以这里改成可滚动，并把按钮放在靠前的位置。
 *
 * 批次 5 之前，这个界面只是骨架：按钮能切换文案，但不驱动真实的接收服务
 * （那要等 JNI 把 castcore 接进来）。
 */
@Composable
fun StandbyScreen() {

    // 电视端默认设备名取设备型号（文档 2.4）。
    val deviceName = remember { Build.MODEL.ifBlank { "Android TV" } }

    var serviceEnabled by remember { mutableStateOf(false) }

    // 电视上用户只能用遥控器，进页面时焦点要落在唯一的按钮上，
    // 否则方向键按下去没有任何反应。
    val buttonFocus = remember { FocusRequester() }

    Surface(modifier = Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                // 矮屏（横屏手机、老电视盒子）上内容会超出，允许滚动。
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 48.dp, vertical = 24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            Text(
                text = deviceName,
                style = MaterialTheme.typography.displaySmall,
                textAlign = TextAlign.Center
            )

            Spacer(modifier = Modifier.height(8.dp))

            Text(
                text = if (serviceEnabled) "等待手机连接…" else "接收服务已关闭",
                style = MaterialTheme.typography.titleLarge,
                textAlign = TextAlign.Center
            )

            // 按钮紧跟状态文字，放在说明之前 —— 横屏时它必须在首屏可见区内。
            Spacer(modifier = Modifier.height(20.dp))

            Button(
                onClick = { serviceEnabled = !serviceEnabled },
                modifier = Modifier.focusRequester(buttonFocus)
            ) {
                Text(
                    text = if (serviceEnabled) "关闭接收服务" else "开启接收服务",
                    style = MaterialTheme.typography.titleMedium
                )
            }

            Spacer(modifier = Modifier.height(24.dp))

            // 连接说明。用户的典型困惑是「手机搜不到电视」，多半是没连同一个
            // Wi-Fi 或者路由器开了 AP 隔离（文档 6.3），所以把提示写在待机页上。
            Text(
                text = "请让手机与本机连接同一个 Wi-Fi，然后：\n" +
                    "iPhone：控制中心 → 屏幕镜像\n" +
                    "Android：视频 App 里的「投屏」按钮",
                style = MaterialTheme.typography.bodyMedium,
                textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth()
            )
        }
    }

    // 进页面就把焦点交给按钮：电视上不这么做的话，用户按遥控器没有任何反馈，
    // 会以为应用卡死了。
    LaunchedEffect(Unit) {
        buttonFocus.requestFocus()
    }
}
