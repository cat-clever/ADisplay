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
 * 批次 0 只呈现待机页；投屏页与前台服务随批次 5 接入。
 */
class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setContent {
            ADisplayTvTheme {
                StandbyScreen()
            }
        }
    }
}
