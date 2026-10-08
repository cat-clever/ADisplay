package com.adisplay.tv

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import com.adisplay.tv.ui.ADisplayTvTheme
import com.adisplay.tv.ui.StandbyScreen

/**
 * 电视端唯一的 Activity。
 *
 * 文档 2.3 要求「投屏开始时自动全屏并置于最上层，结束后回到待机页」，
 * 所以这里只有一个承载待机页与投屏页的容器，不做多 Activity 跳转 ——
 * 电视上遥控器导航比触屏麻烦得多，页面越少越好。
 *
 * 引擎（EngineModel）跟着它的生命周期走：电视上只有一个 Activity、也没有
 * 多窗口，暂时不值得再包一层 Service。接入前台服务是后面批次的事。
 */
class MainActivity : ComponentActivity() {

    private lateinit var model: EngineModel

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 传 applicationContext 而不是 this：模型会活到 onDestroy 之后
        // （组播锁的释放路径上还要用），拿 Activity 的引用容易变成泄漏。
        model = EngineModel(applicationContext)

        setContent {
            ADisplayTvTheme {
                StandbyScreen(model)
            }
        }
    }

    override fun onStart() {
        super.onStart()
        // 只创建引擎、挂回调，不自动开服务：装好就一直在广播的话，同一个
        // Wi-Fi 下谁都能直接投过来。文档 2.3 要的是按一下才开。
        model.attach()
    }

    override fun onStop() {
        // 退到后台就停服务。现在还没有前台服务托底，进程随时会被系统回收，
        // 与其留一个半死不活的广播，不如把组播锁和端口都干干净净地还回去。
        model.stopService()
        super.onStop()
    }

    override fun onDestroy() {
        model.release()
        super.onDestroy()
    }
}
