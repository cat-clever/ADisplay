package com.adisplay.tv.ui

import android.content.Context
import android.content.pm.PackageManager
import android.content.res.Configuration
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.tv.material3.MaterialTheme

/**
 * 待机页的排版参数。
 *
 * 为什么需要它：电视端的屏幕跨度极大 —— 手机横屏约 640dp 宽、1080p 电视
 * 960dp、4K 电视 1920dp。用一套固定字号的结果是：手机上内容溢出（按钮被
 * 挤出屏幕），4K 电视上又小得像蚂蚁。
 *
 * 文档 2.3 的要求是「10 英尺界面：大字号」—— 观看距离远，字号要按屏幕
 * 尺寸放大，而不是按物理尺寸固定。
 *
 * 注意 dp 已经处理了像素密度，所以这里分档依据的是「可用宽度」而不是
 * 分辨率。720p 与 1080p 的电视在 dp 上只差 1.5 倍，不是 2 倍。
 */
@Immutable
data class StandbyLayout(
    val titleStyle: TextStyle,
    val statusStyle: TextStyle,
    val bodyStyle: TextStyle,
    val actionStyle: TextStyle,
    val horizontalPadding: Dp,
    val verticalPadding: Dp,
    /** 标题与状态文字之间。 */
    val spacingTight: Dp,
    /** 状态文字与按钮之间。 */
    val spacingMedium: Dp,
    /** 按钮与说明文字之间。 */
    val spacingLoose: Dp,
    /** 按钮最小宽度。给个下限免得窄屏上文字被挤成两行。 */
    val buttonMinWidth: Dp,
    /**
     * 日志区的固定高度。
     *
     * 给死高度而不是让它按内容撑：日志区钉在待机页底部，内容再长也只占
     * 这一块 —— 否则日志一多会把上面的设备名和按钮整个顶出屏幕，而那两样
     * 是用户唯一要按的东西。
     */
    val logHeight: Dp,
    /** 日志文字。等宽字体，时间戳与级别能按列对齐，扫一眼就能找到 ERROR。 */
    val logStyle: TextStyle,
)

/** 屏幕宽度的分档阈值。 */
private val COMPACT_MAX_WIDTH = 700
private val EXPANDED_MIN_WIDTH = 1100

/**
 * 按屏幕宽度挑一套排版。
 *
 * 三档：
 *   compact   手机横屏、低端盒子（< 700dp）—— 紧凑，保证内容一屏放得下
 *   standard  1080p 电视（700..1100dp）—— 标准 10 英尺排版
 *   expanded  4K 电视（>= 1100dp）—— 放大字号，远距离可读
 */
// 必须标 @Composable：函数体里读了 MaterialTheme.typography，
// 那是 @Composable 属性，从普通函数里访问不到。
@Composable
fun resolveStandbyLayout(screenWidthDp: Int): StandbyLayout {
    return when {
        screenWidthDp < COMPACT_MAX_WIDTH -> StandbyLayout(
            titleStyle = MaterialTheme.typography.headlineLarge,
            statusStyle = MaterialTheme.typography.titleMedium,
            bodyStyle = MaterialTheme.typography.bodySmall,
            actionStyle = MaterialTheme.typography.titleSmall,
            horizontalPadding = 24.dp,
            verticalPadding = 16.dp,
            spacingTight = 6.dp,
            spacingMedium = 14.dp,
            spacingLoose = 16.dp,
            buttonMinWidth = 160.dp,
            logHeight = 84.dp,
            logStyle = MaterialTheme.typography.labelSmall.copy(fontFamily = FontFamily.Monospace),
        )

        screenWidthDp < EXPANDED_MIN_WIDTH -> StandbyLayout(
            titleStyle = MaterialTheme.typography.displaySmall,
            statusStyle = MaterialTheme.typography.titleLarge,
            bodyStyle = MaterialTheme.typography.bodyMedium,
            actionStyle = MaterialTheme.typography.titleMedium,
            horizontalPadding = 48.dp,
            verticalPadding = 24.dp,
            spacingTight = 8.dp,
            spacingMedium = 20.dp,
            spacingLoose = 24.dp,
            buttonMinWidth = 220.dp,
            logHeight = 132.dp,
            logStyle = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
        )

        else -> StandbyLayout(
            titleStyle = MaterialTheme.typography.displayMedium,
            statusStyle = MaterialTheme.typography.headlineMedium,
            bodyStyle = MaterialTheme.typography.titleMedium,
            actionStyle = MaterialTheme.typography.titleLarge,
            horizontalPadding = 96.dp,
            verticalPadding = 48.dp,
            spacingTight = 16.dp,
            spacingMedium = 40.dp,
            spacingLoose = 48.dp,
            buttonMinWidth = 320.dp,
            logHeight = 190.dp,
            logStyle = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace),
        )
    }
}

/**
 * 主要的输入方式。
 *
 * 同一个 APK 要同时跑在电视（只有遥控器）和手机/平板（触屏）上，
 * 两者的交互差异必须处理：
 *
 *   遥控器  进页面就要把焦点交给按钮。没有焦点的话方向键按下去毫无反应，
 *           用户会以为应用卡死。提示文案要写「按遥控器确定键」。
 *   触屏    不能自动聚焦 —— 焦点框会一直挂在按钮上，像是没点干净。
 *           提示文案要写「点击下方按钮」。
 *
 * 判断依据取两条：uiMode 是不是 TELEVISION，以及有没有 LEANBACK 特性。
 * 单看 uiMode 会漏掉部分把电视模式报成 NORMAL 的定制系统；
 * 单看 LEANBACK 会漏掉一些平板上装了电视版桌面的情况。
 */
enum class InputMode { Remote, Touch }

@Composable
fun rememberInputMode(): InputMode {
    val context = LocalContext.current
    return remember(context) { detectInputMode(context) }
}

private fun detectInputMode(context: Context): InputMode {
    val uiMode = context.resources.configuration.uiMode and Configuration.UI_MODE_TYPE_MASK
    val isTelevisionByMode = uiMode == Configuration.UI_MODE_TYPE_TELEVISION

    val isTelevisionByFeature = try {
        context.packageManager.hasSystemFeature(PackageManager.FEATURE_LEANBACK)
    } catch (ignored: Exception) {
        false
    }

    return if (isTelevisionByMode || isTelevisionByFeature) InputMode.Remote else InputMode.Touch
}
