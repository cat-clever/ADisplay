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

    // AirPlay 的 model 与 srcvers。这两个值会影响 iPhone 是否愿意把我们
    // 当作可镜像的目标，批次 4 接 AirPlay 时会按实际协议行为再校准。
    std::string airplay_model = "AppleTV6,2";
    std::string airplay_srcvers = "220.68";

    // SSDP 的 SERVER 头。UPnP/1.0 这个标记不能少，部分手机端靠它识别。
    std::string server_header = "UPnP/1.0 ADisplay/0.1.0";

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
