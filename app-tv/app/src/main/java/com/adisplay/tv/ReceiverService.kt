// ADisplay —— 接收服务的前台服务外壳
//
// 为什么需要它：手机一切到后台（锁屏、去别的应用），整个进程就不再是前台进程，
// 系统随时可以把它回收 —— 表现就是「一退到后台投屏就断了」。前台服务带一条常驻
// 通知，等于告诉系统「这个进程正在干活，别收」，接收才能一直在后台跑。
//
// 它**只负责保活**：引擎在 EngineHolder 里，是进程级的，不跟着 Activity 走。
// 这里不碰协议、不碰界面。
//
// 通知上挂了「停止接收」：那条通知会一直挂在那儿，得给用户一个能收掉它的地方，
// 否则只能回到应用里点按钮。

package com.adisplay.tv

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat

class ReceiverService : Service() {

    override fun onBind(intent: Intent?): IBinder? {
        // 不提供绑定：界面拿引擎走 EngineHolder，不需要跟服务握手。
        return null
    }

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent != null && intent.action == ACTION_STOP) {
            // 通知上那个「停止接收」。停引擎会把服务一并停掉（见 EngineModel
            // 的 stopService），这里再 stopSelf 一次是兜底。
            val model = EngineHolder.peek()
            if (model != null) {
                model.stopService()
            }
            stopSelf()
            return START_NOT_STICKY
        }

        startForegroundCompat(fromBoot = intent?.getBooleanExtra(EXTRA_FROM_BOOT, false) == true)
        // 引擎这时候应该已经在了（服务是跟着引擎起来的）。取一次是兜底：
        // 万一是被系统拉起来的，至少把回调挂上。
        EngineHolder.get(this).attach()
        return START_NOT_STICKY
    }

    /**
     * 报什么前台服务类型，取决于这次是谁把服务拉起来的。
     *
     * **开机自启那一路不能用 mediaPlayback。** Android 15 起，从 BOOT_COMPLETED
     * 接收器里起前台服务有一张禁止名单 —— dataSync、camera、mediaPlayback、
     * phoneCall、mediaProjection、microphone 都在上面，硬起会抛
     * ForegroundServiceStartNotAllowedException（服务起不来、接收也就没了，
     * 而且是在真机上才看得到的静默失败）。所以开机那一路报 connectedDevice：
     * 它不在名单里，而且更贴合服务当时真正在做的事 —— 守着局域网等外部设备
     * （手机）连进来。它要求的条件我们本来就满足（清单里声明了
     * CHANGE_WIFI_MULTICAST_STATE，前台服务权限也申请了）。
     *
     * 用户在界面上按开的那一路仍报 mediaPlayback：那时候确实是要接媒体流。
     *
     * 两个类型都要写进清单的 android:foregroundServiceType，否则 Android 14+
     * 会按「声明的类型」校验，运行时传一个没声明的类型同样抛异常。
     */
    private fun startForegroundCompat(fromBoot: Boolean) {
        // Android 14 起前台服务必须声明类型，且要与清单里 android:foregroundServiceType
        // 写的一致。用 ServiceCompat 包一层：低版本它会自动忽略类型参数。
        ServiceCompat.startForeground(
            this,
            NOTIFICATION_ID,
            buildNotification(),
            if (fromBoot) {
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            } else {
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            },
        )
    }

    private fun buildNotification(): Notification {
        val strings = UiLanguageStore.strings(this)
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val stop = PendingIntent.getService(
            this,
            1,
            Intent(this, ReceiverService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            // 通知文字跟着界面语言走：它可能是用户唯一看得见的界面（服务在跑、Activity
            // 没起来时）。语言存在 UiLanguageStore 里，正因为服务不能指望去问 EngineModel。
            .setContentTitle(strings.notificationTitle)
            .setContentText(strings.notificationText)
            .setContentIntent(open)
            .addAction(0, strings.notificationStop, stop)
            // 常驻、划不掉：它就是「接收还开着」的凭据。能划掉的话，用户看一眼
            // 通知栏没了，会以为服务也停了。
            .setOngoing(true)
            .setShowWhen(false)
            .build()
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return
        }
        val manager = getSystemService(NotificationManager::class.java)
        if (manager == null) {
            return
        }
        if (manager.getNotificationChannel(CHANNEL_ID) != null) {
            return
        }

        val strings = UiLanguageStore.strings(this)
        val channel = NotificationChannel(
            CHANNEL_ID,
            strings.notificationChannelName,
            // LOW：不出声也不弹窗 —— 它是状态说明，不是提醒。
            NotificationManager.IMPORTANCE_LOW,
        )
        channel.description = strings.notificationChannelDescription
        manager.createNotificationChannel(channel)
    }

    companion object {
        private const val CHANNEL_ID = "adisplay-receiver"
        private const val NOTIFICATION_ID = 1

        const val ACTION_STOP = "com.adisplay.tv.action.STOP_RECEIVING"

        /** 这次是开机自启拉起来的（见 startForegroundCompat 里那张禁止名单）。 */
        private const val EXTRA_FROM_BOOT = "com.adisplay.tv.extra.FROM_BOOT"

        /** 起前台服务。已经在跑时再调一次是安全的。 */
        fun start(context: Context, fromBoot: Boolean = false) {
            val intent = Intent(context, ReceiverService::class.java)
            if (fromBoot) {
                intent.putExtra(EXTRA_FROM_BOOT, true)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, ReceiverService::class.java))
        }
    }
}
