// AirPlay 接缝对内的两个只读窗口（实现与理由见 DnssdShim.cpp）。
//
// 协议层把配对公钥和设备身份存在 dnssd_t 里，却不提供读取接口：公钥是
// raop_set_dnssd 塞进去的，设备 id 是 /info 自己格式化出来的。而广播需要
// 这两个值 —— 尤其是设备 id，必须与 /info 那份字字相同，所以不能自己再
// 拼一遍。这里开的这两个窗口返回的就是协议层实际使用的那两份。
#pragma once

#include "dnssd.h"

#ifdef __cplusplus
extern "C" {
#endif

// 配对公钥（十六进制）。协议层还没交出公钥时返回空串，不是 NULL。
const char* adisplay_dnssd_public_key(dnssd_t* dnssd);

// 设备 id 的规范化形式（小写、冒号分隔），与 /info 应答里的 deviceID 相同。
const char* adisplay_dnssd_device_id(dnssd_t* dnssd);

#ifdef __cplusplus
}
#endif
