// ADisplay —— 镜像投屏页
//
// 与 macOS 的投屏页同一套行为：画面满幅，四周不留边距也不留边框 —— 投屏时
// 用户看的就是画面，任何一圈留白都是白扔的像素。
//
// 全屏与退出走 PlaybackFullScreen（点画面、遥控器「返回」、遥控器「菜单」）。
// 镜像页尤其需要它：这里以前没有任何退路 —— 按遥控器的「返回」会一路冒到
// Activity，把整个应用关掉。

package com.adisplay.tv.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.adisplay.tv.EngineModel

@Composable
fun MirrorScreen(
    model: EngineModel,
    onExit: () -> Unit,
    onShowLog: () -> Unit,
    /// false 表示用户点了「结束投屏」：这一页还在树上（画面与解码器都留着），
    /// 只是不占屏、不接键。见下面那段注释。
    active: Boolean = true,
) {
    PlaybackFullScreen(
        title = LocalStrings.current.mirroringTitle(model.deviceName),
        onExit = onExit,
        onShowLog = onShowLog,
        active = active,
    ) {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center,
        ) {
            // 按解码器报出来的尺寸做信箱式留边。
            //
            // 少了这一步，SurfaceView 铺满父容器，而解码器是**按 Surface 的
            // 宽高比拉伸画面**的 —— 竖屏设备上会把画面横向拉宽四成，看起来
            // 就是「比例不对」。等它报出尺寸之前先铺满，报出来再收紧。
            // 不 active（用户点了「结束投屏」）时把画面缩成 1dp、挪到右下角。
            //
            // **关键是要留在树上**：一离开组合，SurfaceView 的 surface 就被销毁，
            // MirrorVideoPlayer.detach() 会把解码器一并释放 —— 于是「继续观看」回来
            // 是**新建解码器**，而新解码器必须等到发送端下一个 I 帧才出画面（手机画面
            // 静止时 iOS 可能很久不插 I 帧），用户看到的就是「一直黑屏但有声音」。
            // 1dp 小到看不见，但这个 surface 一直算"可见"，系统不会销毁它。
            if (!active) {
                MirrorSurface(
                    player = model.mirrorPlayer,
                    modifier = Modifier
                        .align(Alignment.BottomEnd)
                        .size(1.dp),
                )
                return@Box
            }

            val size = model.mirrorPlayer.videoSize
            val shape = if (size != null && size.first > 0 && size.second > 0) {
                Modifier.aspectRatio(size.first.toFloat() / size.second.toFloat())
            } else {
                Modifier.fillMaxSize()
            }
            MirrorSurface(player = model.mirrorPlayer, modifier = shape)
        }
    }
}
