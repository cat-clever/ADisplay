// ADisplay —— 镜像画面的显示面（Compose 包装）
//
// 用 SurfaceView 而不是别的：MediaCodec 要的是一块能直接写的 Surface，而
// SurfaceView 的 holder 正好给这个 —— 解码器把画面写进去、系统合成上屏，
// 中间不经过我们的代码，也就不经过 Compose 的重组。
//
// 这一点在电视上尤其要紧：每秒几十帧如果都走 Compose 状态，界面会被重组压垮。
// 与 macOS 那边把帧直接交给显示层、不进 @Published 是同一个道理。

package com.adisplay.tv.ui

import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import com.adisplay.tv.MirrorVideoPlayer

/**
 * 把解码器的输出铺在屏幕上。
 *
 * 渲染面的出现与消失都由 SurfaceView 的生命周期驱动：面建好就挂上解码器，
 * 面没了就摘下来（切页面、Activity 暂停都会走到）。
 */
@Composable
fun MirrorSurface(player: MirrorVideoPlayer, modifier: Modifier = Modifier) {
    AndroidView(
        modifier = modifier,
        factory = { context ->
            SurfaceView(context).apply {
                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(holder: SurfaceHolder) {
                        player.attach(holder.surface)
                    }

                    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                        // 尺寸变化由解码器按 SPS 自己处理，这里不需要做什么。
                    }

                    override fun surfaceDestroyed(holder: SurfaceHolder) {
                        player.detach()
                    }
                })
            }
        },
    )
}
