// ADisplay —— Android 上的 mDNS 广播
//
// 核心在桌面上自己发 mDNS（macOS 用系统 Bonjour、Windows 用 DNS-SD），Android
// 上发不了：NsdManager 只在 Java 层，NDK 里没有等价接口，而自己抢 5353 端口
// 实现一份 mDNS 又要和系统的省电策略、MulticastLock 较劲 —— 那条路是文档 4.5
// 明确要避免的。所以广播由界面层接手（见 platform/android/src/MdnsPublisherAndroid.cpp）。
//
// 广播**内容**由核心给（ad_engine_get_airplay_advert）：features、pk、deviceid
// 这些必须与协议层的 /info 应答逐字一致 —— 写成两份迟早对不上，而对不上的症状
// 极具误导性：「iPhone 里看不到这台设备」，或者看得见却配不上对。
//
// 要注册两条：_airplay._tcp 与 _raop._tcp。后者是为了音频流，实例名有固定格式
// <deviceid>@<显示名>，iOS 靠它把音频关联回 AirPlay 那条记录。

package com.adisplay.tv

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo

class MdnsAdvertiser(private val context: Context, private val onNotice: (String) -> Unit) {

    private val nsd: NsdManager? =
        context.getSystemService(Context.NSD_SERVICE) as? NsdManager
    private val registrations = mutableListOf<NsdManager.RegistrationListener>()

    /** 按核心给的广播数据注册服务。可以在服务每次启动后调用。 */
    fun publish(advert: String) {
        withdraw()
        if (nsd == null) {
            onNotice("mDNS 广播：这台设备没有可用的 NsdManager，手机端将看不到本机")
            return
        }
        if (advert.isEmpty()) {
            onNotice("mDNS 广播：没拿到广播数据，手机端将看不到本机")
            return
        }

        var port = 0
        var airplayName: String? = null
        var raopName: String? = null
        val airplayAttributes = LinkedHashMap<String, ByteArray>()
        val raopAttributes = LinkedHashMap<String, ByteArray>()

        for (line in advert.split('\n')) {
            if (line.isEmpty()) {
                continue
            }
            val equal = line.indexOf('=')
            if (equal <= 0) {
                continue
            }
            val key = line.substring(0, equal)
            val value = line.substring(equal + 1)
            when {
                key == "port" -> port = value.toIntOrNull() ?: 0
                key == "airplay_name" -> airplayName = value
                key == "raop_name" -> raopName = value
                key.startsWith("airplay.") ->
                    hexToBytes(value)?.let { airplayAttributes[key.removePrefix("airplay.")] = it }
                key.startsWith("raop.") ->
                    hexToBytes(value)?.let { raopAttributes[key.removePrefix("raop.")] = it }
            }
        }

        register("_airplay._tcp", airplayName, port, airplayAttributes)
        register("_raop._tcp", raopName, port, raopAttributes)
    }

    /** 撤下全部注册。停服务、退出应用时调用。 */
    fun withdraw() {
        val manager = nsd
        if (manager != null) {
            for (listener in registrations) {
                try {
                    manager.unregisterService(listener)
                } catch (ignored: IllegalArgumentException) {
                    // 没注册成功过的监听器在这里会抛，忽略即可 —— 目的就是让它不在了。
                }
            }
        }
        registrations.clear()
    }

    private fun register(
        serviceType: String,
        serviceName: String?,
        port: Int,
        attributes: Map<String, ByteArray>,
    ) {
        val manager = nsd
        if (manager == null || serviceName.isNullOrEmpty() || port <= 0) {
            onNotice("mDNS 广播：" + serviceType + " 的参数不全，手机端可能看不到本机")
            return
        }

        val info = NsdServiceInfo()
        info.serviceType = serviceType
        info.serviceName = serviceName
        info.port = port
        for ((key, value) in attributes) {
            // 空值的属性会被 NsdManager 拒绝，跳过 —— 少一条 TXT 好过整条注册不上。
            if (value.isEmpty()) {
                continue
            }
            info.setAttribute(key, value)
        }

        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(registered: NsdServiceInfo) {
                onNotice("mDNS 已公布 " + serviceType + " / " + registered.serviceName)
            }

            override fun onRegistrationFailed(failed: NsdServiceInfo, errorCode: Int) {
                onNotice("mDNS 公布失败：" + serviceType + "（错误 " + errorCode + "）")
            }

            override fun onServiceUnregistered(unregistered: NsdServiceInfo) {
                // 主动撤回，不需要提示。
            }

            override fun onUnregistrationFailed(unregistered: NsdServiceInfo, errorCode: Int) {
                onNotice("mDNS 撤回失败：" + serviceType + "（错误 " + errorCode + "）")
            }
        }

        manager.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener)
        registrations.add(listener)
    }

    /** 把核心给的十六进制解回字节。奇数长度或含非法字符时返回 null。 */
    private fun hexToBytes(text: String): ByteArray? {
        if (text.length % 2 != 0) {
            return null
        }
        val out = ByteArray(text.length / 2)
        var index = 0
        while (index < text.length) {
            val high = Character.digit(text[index], 16)
            val low = Character.digit(text[index + 1], 16)
            if (high < 0 || low < 0) {
                return null
            }
            out[index / 2] = ((high shl 4) or low).toByte()
            index += 2
        }
        return out
    }
}
