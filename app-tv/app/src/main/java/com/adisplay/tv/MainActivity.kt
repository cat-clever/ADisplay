package com.adisplay.tv

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import android.view.KeyEvent
import android.view.WindowManager
import androidx.activity.compose.setContent
import androidx.compose.runtime.LaunchedEffect
import com.adisplay.tv.ui.ADisplayTvTheme
import com.adisplay.tv.ui.StandbyScreen

/**
 * 电视端唯一的 Activity。
 *
 * 文档 2.3 要求「投屏开始时自动全屏并置于最上层，结束后回到待机页」，
 * 所以这里只有一个承载待机页与投屏页的容器，不做多 Activity 跳转 ——
 * 电视上遥控器导航比触屏麻烦得多，页面越少越好。
 *
 * 引擎（EngineModel）**不**跟着它的生命周期走，而是进程级的（见 EngineHolder）：
 * 接收服务要在应用退到后台之后继续跑，而 Activity 会被销毁重建，引擎挂在它身上
 * 就跟着一起没了。保活由 ReceiverService 那个前台服务负责 —— 这个 Activity 只
 * 是界面，关掉它不影响投屏。
 */
/** 申请通知权限的请求码。只用来区分回调，值本身没有意义。 */
private const val REQUEST_NOTIFICATIONS = 1

class MainActivity : ComponentActivity() {

    private lateinit var model: EngineModel

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 进程级引擎。传 applicationContext 让它活到 Activity 之后 —— 现在它
        // 确实会活得更久（服务还在跑时 Activity 可能已经销毁了）。
        model = EngineHolder.get(this)

        // Android 13 起，前台服务那条常驻通知要用户允许通知才显示得出来。
        // 拒绝也不影响接收：服务照样跑，只是通知栏里没有它。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
            != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQUEST_NOTIFICATIONS)
        }

        setContent {
            ADisplayTvTheme {
                StandbyScreen(model)

                // 投屏期间别让屏幕自动息屏 —— 用户看的就是画面，中途黑掉等于
                // 中断。只在真的在投（镜像会话或媒体会话）时加这个标志：
                // 待机时该省电还是省电，电视盒子常年开着，这一条对它也重要。
                val casting = model.mirrorSessionId != null || model.playingMedia != null
                LaunchedEffect(casting) {
                    if (casting) {
                        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                    } else {
                        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                    }
                }
            }
        }
    }

    /**
     * 遥控器的「菜单」键：投屏中按它就结束本次投屏。
     *
     * 放在 Activity 里而不是播放页里，是因为播放页里那层接键的控件只处理方向键与
     * 确定键、把「菜单」放过去（见 PlaybackFullScreen 的 onKeyEvent）—— 按键先走
     * 视图树，没被消费才轮到这里的 onKeyDown。
     */
    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val casting = model.mirrorSessionId != null || model.playingMedia != null
        if (keyCode == KeyEvent.KEYCODE_MENU && casting) {
            // 与界面上的「结束投屏」一致：回主界面，不停服务。
            model.dismissCasting()
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onStart() {
        super.onStart()
        // 只创建引擎、挂回调，不自动开服务：装好就一直在广播的话，同一个
        // Wi-Fi 下谁都能直接投过来。文档 2.3 要的是按一下才开。
        model.attach()
    }

    override fun onStop() {
        // 这里**刻意**不停服务：切应用、锁屏之后投屏要继续，这正是前台服务存在的
        // 理由。以前这里停服务，表现就是「一退到后台投屏自己就断了」。
        // 要停只有两条路：界面上点「关闭接收服务」，或通知上的「停止接收」。
        super.onStop()
    }

    override fun onDestroy() {
        // 也不销毁引擎：Activity 只是界面，接收服务可能还在跑，两者生命周期已经
        // 分开了（见 EngineHolder）。引擎与进程同寿。
        super.onDestroy()
    }
}
