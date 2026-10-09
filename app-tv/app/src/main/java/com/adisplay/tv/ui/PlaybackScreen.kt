package com.adisplay.tv.ui

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.viewinterop.AndroidView
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.ui.PlayerView
import com.adisplay.tv.AdPlaybackCommand
import com.adisplay.tv.AdTransportState
import com.adisplay.tv.EngineModel
import com.adisplay.tv.PlaybackIntent
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collect

/** 回报间隔。1 秒与 UPnP 的 GENA 事件、手机端进度条的刷新节奏相当。 */
private const val REPORT_INTERVAL_MS = 1000L

/**
 * 投屏播放页。
 *
 * 为什么用 ExoPlayer 而不是给核心喂帧：DLNA / AirPlay 视频推送给的是 URL
 * （见 adisplay.h 的 on_media_url），拉流、解复用、硬解、音画同步这一整套
 * 自己写必然做不过 Media3，而 Media3 已经带了电视上最需要的那些格式支持。
 *
 * 页面本身只做三件事：
 *   1. 把 URL 交给播放器起播；
 *   2. 把手机发来的控制意图落到播放器上；
 *   3. 每秒把播放器的真实状态回报给核心 —— 手机端的进度条和音量靠它走。
 *
 * 播放期间是全屏的，屏幕上不留任何常驻控件，退路全部交给 PlaybackFullScreen：
 * 点画面、遥控器「返回」、遥控器「菜单」，三条都结束本次投屏。电视只有遥控器，
 * 一旦卡在播放页又没有可见的按钮，用户唯一的办法是拔电源 —— 所以退路要给够。
 */
@Composable
fun PlaybackScreen(
    model: EngineModel,
    sessionId: Int,
    url: String,
    onExit: () -> Unit,
) {
    val context = LocalContext.current
    // 播放器只在进入本页时建一次。remember 把它钉在本次组合里，重组（日志展开、
    // 焦点变化、每秒回报引起的状态变化）都直接复用同一个实例 —— 每次重组都
    // 新建的话画面会一直黑、音频还会叠着响。
    val exoPlayer = remember { ExoPlayer.Builder(context).build() }

    // 离开本页（会话结束、切回待机页、Activity 销毁）时归还解码器。电视上
    // 可用的硬解实例很少，漏放一个后面就可能起不来流。
    DisposableEffect(exoPlayer) {
        onDispose { exoPlayer.release() }
    }

    // 播放器里表达不出来的那几项状态，只能自己记着。
    val playerState = remember { PlaybackUiState() }

    // 收到 URL 就起播。键取 url 而不是 player：手机可能在同一次会话里接着
    // 推第二段视频，那时播放器还是同一个，得重新 setMediaItem。
    LaunchedEffect(exoPlayer, url) {
        exoPlayer.setMediaItem(MediaItem.fromUri(url))
        exoPlayer.prepare()
        exoPlayer.play()
        playerState.stopped = false
        reportPlayback(model, exoPlayer, sessionId, playerState)
    }

    // 控制意图。协程跟着页面生命周期走：页面一离开组合就自动取消，
    // 不会有回调打到已经 release 的播放器上。
    LaunchedEffect(exoPlayer, sessionId) {
        model.playbackIntents.collect { intent ->
            // 迟到的命令（属于已经结束的上一个会话）不能落到这个播放器上。
            if (intent.sessionId != sessionId) return@collect
            applyIntent(exoPlayer, intent, playerState)
            // 命令生效后立刻回报一次。只等下一秒那次定时回报的话，用户拖完
            // 进度条，手机上的进度条会先弹回原处再跳过去。
            reportPlayback(model, exoPlayer, sessionId, playerState)
        }
    }

    // 每秒回报一次。手机端拿它更新进度条、音量滑块和播放/暂停按钮。
    LaunchedEffect(exoPlayer, sessionId) {
        while (true) {
            reportPlayback(model, exoPlayer, sessionId, playerState)
            delay(REPORT_INTERVAL_MS)
        }
    }

    // 全屏，并接上三条退路：点画面、遥控器「返回」、遥控器「菜单」。
    PlaybackFullScreen(onExit = onExit) {
        AndroidView(
            factory = { ctx ->
                PlayerView(ctx).apply {
                    // 控制条留给遥控器：按「确定」调出播放/暂停与进度。
                    // 触摸则归上面那层透明触摸层管 —— 点一下就退出全屏。
                    // 全屏期间不常驻任何控件，是用户明确要的。
                    useController = true
                    controllerAutoShow = true
                    // 起播缓冲时转圈。电视上拉流慢是常事，黑屏和「正在缓冲」
                    // 在用户眼里是两回事。
                    setShowBuffering(PlayerView.SHOW_BUFFERING_WHEN_PLAYING)
                    // 放视频时别让系统息屏。
                    keepScreenOn = true
                    player = exoPlayer
                }
            },
            modifier = Modifier.fillMaxSize(),
        )
    }
}

/**
 * 播放器里表达不出来、界面自己记着的那几项。
 *
 * 不放进 Compose 的 snapshot 状态：它们只被事件处理器和回报循环读写，
 * 从不参与界面绘制，做成可变状态反而会引来多余的重组。
 */
private class PlaybackUiState {
    /** 音量百分比。静音时播放器音量是 0，但这个值要留着 —— 取消静音得还给用户。 */
    var volumePercent = 100

    /**
     * 静音开关。ExoPlayer 没有静音这个概念，静音只能靠把音量压到 0 实现，
     * 所以「是静音」还是「用户把音量调到 0」只能自己分清楚：回报给核心的
     * 是两条独立通道（volume 与 muted），混在一起手机会显示错。
     */
    var muted = false

    /**
     * 手机按过「停止」。
     *
     * ExoPlayer 没有「停止」这个状态，只能表现成「回到起点并暂停」，单看
     * 播放器分不出「停止后停在 0」和「用户拖到 0 然后暂停」。回报时要说成
     * STOPPED，否则手机上的停止按钮会自己弹回「暂停」。
     */
    var stopped = false
}

/** 把一条控制意图落到播放器上。 */
private fun applyIntent(player: ExoPlayer, intent: PlaybackIntent, state: PlaybackUiState) {
    when (intent.command) {
        AdPlaybackCommand.PLAY -> {
            player.play()
            state.stopped = false
        }

        AdPlaybackCommand.PAUSE -> {
            player.pause()
            state.stopped = false
        }

        AdPlaybackCommand.STOP -> {
            // 「停止播放」不是「结束投屏」：媒体还挂在会话上，手机随后可以再发
            // Play，所以这里回到起点并暂停。用 player.stop() 会把媒体项清掉，
            // 下一次 Play 就没东西可播了（还得重新 setMediaItem，而地址要从
            // 核心再要一次，核心不会重发）。
            player.pause()
            player.seekTo(0L)
            state.stopped = true
        }

        AdPlaybackCommand.SEEK -> {
            player.seekTo(intent.value)
            state.stopped = false
        }

        AdPlaybackCommand.SET_VOLUME -> {
            state.volumePercent = intent.value.coerceIn(0L, 100L).toInt()
            // 静音期间只更新记下的值，不动播放器音量：音量与静音在 UPnP 里是
            // 两条独立通道，手机上拖音量条不该顺手把静音解掉。
            applyVolume(player, state)
        }

        AdPlaybackCommand.SET_MUTE -> {
            state.muted = intent.value != 0L
            applyVolume(player, state)
        }
    }
}

private fun applyVolume(player: ExoPlayer, state: PlaybackUiState) {
    player.volume = if (state.muted) 0f else state.volumePercent / 100f
}

/** 回报播放器当前状态给核心。每秒一次，另外每次控制意图之后也来一次。 */
private fun reportPlayback(
    model: EngineModel,
    player: ExoPlayer,
    sessionId: Int,
    state: PlaybackUiState,
) {
    val duration = player.duration
    model.reportPlayback(
        sessionId = sessionId,
        transport = transportStateOf(player, state),
        positionMs = player.currentPosition.coerceAtLeast(0L),
        // 时长未知时 ExoPlayer 返回 C.TIME_UNSET —— 一个极大的负数，直接传下去
        // 手机会显示成天文数字。按核心的约定，未知一律传 -1。
        durationMs = if (duration == C.TIME_UNSET || duration < 0L) -1L else duration,
        volume = state.volumePercent,
        muted = if (state.muted) 1 else 0,
    )
}

/**
 * 把播放器状态映射成 AdTransportState。
 *
 * 顺序不能换：缓冲 / 拖动中要优先报 Transitioning，那时候 playWhenReady 可能
 * 还是 true，先判 isPlaying 就会把「正在起播」报成「在播」，手机上的进度条
 * 会开始走而画面上还是黑屏。
 */
private fun transportStateOf(player: ExoPlayer, state: PlaybackUiState): AdTransportState {
    if (player.playbackState == Player.STATE_BUFFERING) return AdTransportState.TRANSITIONING
    if (player.playbackState == Player.STATE_ENDED) return AdTransportState.STOPPED
    if (player.isPlaying) return AdTransportState.PLAYING
    // IDLE 表示播放器上根本没有媒体（还没 prepare、或者出错被重置了）。
    if (player.playbackState == Player.STATE_IDLE) return AdTransportState.NO_MEDIA_PRESENT
    if (state.stopped) return AdTransportState.STOPPED
    return AdTransportState.PAUSED
}
