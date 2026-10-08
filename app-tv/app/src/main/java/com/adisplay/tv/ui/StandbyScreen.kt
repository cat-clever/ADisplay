package com.adisplay.tv.ui

import android.os.Build
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
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
 * 批次 0 的数据是占位的：设备名取电视型号（文档 2.4 规定电视端默认名取设备型号），
 * IP 与真实服务状态待批次 1 接入 DiscoveryService 后从 castcore 取。
 */
@Composable
fun StandbyScreen() {

    // 电视端默认设备名取设备型号（文档 2.4）。
    val deviceName = remember { Build.MODEL.ifBlank { "Android TV" } }

    var serviceEnabled by remember { mutableStateOf(false) }

    Surface(modifier = Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontal = 64.dp, vertical = 48.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            Text(
                text = deviceName,
                style = MaterialTheme.typography.displayMedium,
                textAlign = TextAlign.Center
            )

            Spacer(modifier = Modifier.height(16.dp))

            Text(
                text = if (serviceEnabled) "等待手机连接…" else "接收服务已关闭",
                style = MaterialTheme.typography.headlineSmall,
                textAlign = TextAlign.Center
            )

            Spacer(modifier = Modifier.height(32.dp))

            // 连接说明。用户的典型困惑是「手机搜不到电视」，多半是没连同一个
            // Wi-Fi 或者路由器开了 AP 隔离（文档 6.3），所以把提示写在待机页上。
            Text(
                text = "请让手机与本机连接同一个 Wi-Fi，然后：\n" +
                    "iPhone：控制中心 → 屏幕镜像\n" +
                    "Android：视频 App 里的「投屏」按钮",
                style = MaterialTheme.typography.bodyLarge,
                textAlign = TextAlign.Center
            )

            Spacer(modifier = Modifier.height(48.dp))

            Button(onClick = { serviceEnabled = !serviceEnabled }) {
                Text(
                    text = if (serviceEnabled) "关闭接收服务" else "开启接收服务",
                    style = MaterialTheme.typography.titleMedium
                )
            }
        }
    }
}
