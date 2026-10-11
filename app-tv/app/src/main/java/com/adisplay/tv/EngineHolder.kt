// ADisplay —— 引擎的持有者（进程级）
//
// 为什么引擎不放在 Activity 里：接收服务要能在应用退到后台之后继续跑 ——
// 切应用、锁屏都不该把投屏断掉。而 Activity 会被销毁重建，引擎挂在它身上就
// 跟着一起没了。
//
// 保活由 ReceiverService 那个前台服务负责，这里只管一件事：**引擎只有一个**，
// 界面与服务拿到的是同一个。

package com.adisplay.tv

import android.content.Context

object EngineHolder {

    private var instance: EngineModel? = null

    /** 取引擎，没有就建一个。只在主线程调用。 */
    fun get(context: Context): EngineModel {
        val existing = instance
        if (existing != null) {
            return existing
        }
        val created = EngineModel(context.applicationContext)
        instance = created
        return created
    }

    /**
     * 取引擎，没有就返回 null。
     *
     * 给「只是要停掉它」的路径用（通知上那个「停止接收」）：那种场合不该为了
     * 停一个不存在的引擎而把它建出来。
     */
    fun peek(): EngineModel? {
        return instance
    }
}
