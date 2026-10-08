// ADisplay —— 设备唯一标识（文档 2.4「标识不变」）
//
// 文档原文：只更改显示名称，设备唯一标识（deviceid / UUID）和已配对记录保持不变，
// 已连接过的手机不需要重新配对。
//
// 所以 deviceid 一旦生成就必须持久化，改名、重启、升级都不能变 —— 变了手机端
// 会认为是新设备，得重新配对。
//
// AirPlay 的 TXT 记录里 deviceid 是 MAC 风格字符串（"AA:BB:CC:DD:EE:FF"），
// 因此用 6 字节随机地址；同时另存一个 RFC 4122 UUID 供 DLNA 的 UDN 使用。
#pragma once

#include <cstdint>
#include <string>

namespace adisplay::common {

class Config;

class DeviceIdentity {
public:
    // 从配置读取；缺失或格式非法则生成新的一份。
    // 生成的标识不会自动写盘，调用方需再调 store()（或 ad_engine_save_config）。
    static DeviceIdentity load_or_create(const Config& config);

    // 写回配置对象（真正的落盘由 Config::save 完成）。
    void store(Config& config) const;

    // MAC 风格，形如 "AA:BB:CC:DD:EE:FF"，AirPlay TXT 记录用。
    const std::string& device_id() const { return device_id_; }

    // RFC 4122 v4，形如 "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"，DLNA UDN 用。
    const std::string& uuid() const { return uuid_; }

    // 仅用于测试与排障：从给定字节构造。
    static DeviceIdentity from_bytes(const uint8_t* six_bytes, const uint8_t* sixteen_bytes);

private:
    std::string device_id_;
    std::string uuid_;
};

// 生成一个 MAC 风格标识。
// 第一个字节会清掉多播位、置上「本地管理」位（0x02），
// 这样不会和局域网里真实网卡的 MAC 撞车。
std::string generate_device_id();

// 生成 RFC 4122 v4 UUID 字符串（小写）。
std::string generate_uuid_v4();

// 校验字符串是否是合法的 MAC 风格标识。
bool is_valid_device_id(const std::string& text);

// 校验字符串是否是合法的 RFC 4122 UUID。
bool is_valid_uuid(const std::string& text);

}  // namespace adisplay::common
