// ADisplay —— 服务发现统一管理
//
// 文档 4.2 对它的职责定义：统一管理 mDNS 注册与 SSDP 应答，
// 网卡变化（切换 Wi-Fi、插拔网线）时自动重新注册。
//
// 这一层把「手机怎么看到我们」这件事收在一处：
//   * AirPlay  —— mDNS 公布 _airplay._tcp 与 _raop._tcp，带 TXT 能力记录
//   * DLNA     —— SSDP 应答 M-SEARCH，LOCATION 指向设备描述 XML
//   * 自研协议 —— mDNS 公布 _castpc._tcp
//
// 改名走的是文档 2.4「即时生效」那条路：全部注销再全部重注册，
// 不重启服务，deviceid 与 uuid 保持不变，已配对的手机不需要重新配对。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace adisplay::discovery {

// 公布服务所需的一切。由 AdEngine 从配置与 DeviceIdentity 组装。
struct DiscoveryConfig {
    // 显示名，即手机投屏列表里看到的那个（文档 2.4）。1–32 码点，调用方负责校验。
    std::string device_name;

    // AirPlay 的 deviceid，MAC 风格。取自 DeviceIdentity。
    std::string device_id;

    // DLNA 的 UDN（不带 "uuid:" 前缀）与 AirPlay 的 pi 记录，取自 DeviceIdentity。
    std::string uuid;

    uint16_t airplay_port = 7000;
    uint16_t dlna_port = 49152;
    uint16_t castpc_port = 8765;

    // AirPlay 的 model 与 srcvers。
    //
    // 这里的默认值只是为了「没有 AirPlay 协议层时也有个合法值」——
    // 真正广播时 AdEngine 会用协议层自己的 GLOBAL_MODEL / GLOBAL_VERSION
    // 覆盖它们。必须覆盖：/info 应答里的 model 与 sourceVersion 就是那两个
    // 常量，广播若用别的值，iOS 会把同一台设备当成两台。
    std::string airplay_model = "AppleTV3,2";
    std::string airplay_srcvers = "220.68";

    // AirPlay 接收端的 Ed25519 公钥（十六进制串），由协议层生成后交给这里。
    //
    // 它必须出现在 _airplay._tcp 与 _raop._tcp 的 TXT 记录里：iOS 的
    // pair-verify 要用它核对接收端身份，缺失的表现是设备可见但连不上。
    // 留空表示协议层未就绪，那种情况广播会缺 pk。
    std::string airplay_pk;

    // SSDP 的 SERVER 头。
    //
    // 格式是「<OS>/<版本> UPnP/1.0 <产品>/<版本>」，三部分都不能少：
    //   * UPnP/1.0 是能力标记，缺了多数客户端直接忽略这个设备
    //   * 开头的 OS 部分是 UPnP 规范要求的，缺了部分客户端（实测有国产
    //     浏览器）会解析失败并跳过设备
    // 由 AdEngine 在启动时填入真实的系统信息。
    std::string server_header;

    bool enable_airplay = true;
    bool enable_dlna = true;
    bool enable_castpc = true;

    // 优先使用的网卡地址。留空表示自动选择（跳过回环与虚拟网卡，见文档 6.3）。
    std::string preferred_address;
};

class DiscoveryService {
public:
    DiscoveryService();
    ~DiscoveryService();

    DiscoveryService(const DiscoveryService&) = delete;
    DiscoveryService& operator=(const DiscoveryService&) = delete;

    // 开始广播。任一协议公布失败都会记日志，但只要还有一个成功就返回 true ——
    // 比如 AirPlay 的 mDNS 挂了，DLNA 仍然应该能工作。
    // 全部失败返回 false，out_error 里是主要原因。
    bool start(const DiscoveryConfig& config, std::string* out_error);

    // 停止全部广播，并发出 byebye / goodbye 让手机立刻把设备移出列表。
    void stop();

    // 修改设备名并即时生效（文档 2.4）：注销全部再重新公布，
    // 不重启服务，deviceid / uuid / 已配对记录都不变。
    // 名称不合规返回 false，此时服务保持原状。
    bool set_device_name(const std::string& name, std::string* out_error);

    // 网络变化时调这个。会重新探测本机地址，地址变了才真正重注册 ——
    // 否则每次网卡抖动都重注册一遍，手机端列表会闪。
    void refresh();

    bool is_running() const;

    // 当前用于 LOCATION / A 记录的地址。没有可用地址时返回空串。
    std::string current_address() const;

    // 最近一次公布失败的原因，供界面显示。无错误时为空。
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace adisplay::discovery
