// ADisplay —— 开机自启
//
// 文档 2.3 的电视端要求：「开机自启并以前台服务常驻」。
//
// 这件事对电视盒子比对别的设备都重要：停电之后盒子自己会重启，而重启完得有人拿起
// 遥控器把服务再按开一次，这台电视才算又能收投屏。有了它，恢复供电就自动接上。
//
// 它**不**无条件把服务打开 —— 只有上次是开着的时候才恢复（用户明确按过「关闭接收
// 服务」的话，重启后仍然是关的）。理由与待机页上那条注释一致：装好就一直在广播的话，
// 同一个 Wi-Fi 下谁都能直接投过来，所以这件事必须由用户的选择决定。
//
// 起前台服务这件事有版本规矩，写在 ReceiverService.startForegroundCompat 的注释里
// （Android 15 起 BOOT_COMPLETED 不允许起 mediaPlayback 类型）—— 所以这里只是转手，
// 报什么类型由服务自己按来源决定。

package com.adisplay.tv

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

class BootReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        // 清单里只注册了 BOOT_COMPLETED，但显式判一次：将来若为了别的目的加上
        // 别的 action，不至于顺手把服务也拉起来。
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) {
            return
        }
        // 引擎与是否该起来这件事都在 EngineModel 里，这里只做转手。
        EngineHolder.get(context).restoreServiceAfterBoot()
    }
}
