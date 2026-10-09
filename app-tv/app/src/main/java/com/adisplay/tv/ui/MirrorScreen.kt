// ADisplay —— 镜像投屏页
//
// 与 macOS 的投屏页同一套行为：画面满幅，四周不留边距也不留边框 —— 投屏时
// 用户看的就是画面，任何一圈留白都是白扔的像素。控制条紧贴在画面下方。

package com.adisplay.tv.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.unit.dp
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text
import com.adisplay.tv.EngineModel

@Composable
fun MirrorScreen(model: EngineModel, onExit: () -> Unit) {
    val stopFocus = remember { FocusRequester() }
    val configuration = LocalConfiguration.current
    val layout = resolveStandbyLayout(configuration.screenWidthDp)

    Column(modifier = Modifier.fillMaxSize().background(Color.Black)) {
        // 画面吃掉剩下的全部高度；控制条按内容取高。
        Box(
            modifier = Modifier.fillMaxWidth().weight(1f),
            contentAlignment = Alignment.Center,
        ) {
            // 按解码器报出来的尺寸做信箱式留边。
            //
            // 少了这一步，SurfaceView 铺满父容器，而解码器是**按 Surface 的
            // 宽高比拉伸画面**的 —— 竖屏设备上会把画面横向拉宽四成，看起来
            // 就是「比例不对」。等它报出尺寸之前先铺满，报出来再收紧。
            val size = model.mirrorPlayer.videoSize
            val shape = if (size != null && size.first > 0 && size.second > 0) {
                Modifier.aspectRatio(size.first.toFloat() / size.second.toFloat())
            } else {
                Modifier.fillMaxSize()
            }
            MirrorSurface(player = model.mirrorPlayer, modifier = shape)
        }

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 24.dp, vertical = 16.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.Center,
        ) {
            Text(
                text = "正在镜像屏幕",
                style = MaterialTheme.typography.bodyLarge,
                color = Color.White.copy(alpha = 0.85f),
            )

            Spacer(modifier = Modifier.width(24.dp))

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
