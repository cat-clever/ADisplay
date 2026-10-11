// ADisplay —— 界面文字的提供者（Compose 的 CompositionLocal）

package com.adisplay.tv.ui

import androidx.compose.runtime.staticCompositionLocalOf
import com.adisplay.tv.ChineseStrings
import com.adisplay.tv.Strings

/**
 * 当前语言对应的那份界面文字。
 *
 * 用 CompositionLocal 而不是给每个 composable 加一个 `strings` 形参：待机页往下一路
 * 有六七层（日志抽屉、改名抽屉、悬浮控件、播放页…），其中好几层本来就不需要
 * [com.adisplay.tv.EngineModel]（参数只有要显示的行和几个回调）。逐个加形参的话，
 * 每加一句话就要动四五个签名；而这里在根上提供一次，谁要谁取。
 *
 * 用 static 而不是普通的 compositionLocalOf：语言切换时**整棵子树都要按新文字重建**，
 * 这正是 static 的语义（普通版只重建真正读过它的那部分，容易漏掉某处缓存了字符串的
 * 地方）。代价是切换时多重组一些东西 —— 而切换语言是极低频操作，值。
 *
 * 根上唯一提供它的是 [StandbyScreen]（拿 model.strings）。默认值给中文：万一某处
 * 忘了提供，界面上出现的是中文而不是空字符串 —— 中文是默认语言，这个降级最不意外。
 */
val LocalStrings = staticCompositionLocalOf<Strings> { ChineseStrings }
