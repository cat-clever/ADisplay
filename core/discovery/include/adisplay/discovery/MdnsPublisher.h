// ADisplay —— mDNS 服务发布接口
//
// 文档 4.5 的多平台适配方案：
//   macOS    系统自带 Bonjour（dns_sd.h）
//   Windows  系统 DNS-SD API（windns.h 的 DnsServiceRegister）
//   Linux    Avahi 的 libdns_sd 兼容层（P2，暂缓）
//
// 为什么不自己实现一份 mDNS：
//   可行，而且一份代码就能跨平台。但 macOS 上系统已经有 mDNSResponder 常驻
//   并占着 5353 端口，走 dns_sd API 是让它替我们处理报文编码、冲突检测、
//   缓存一致性这些细节；自己抢 SO_REUSEPORT 去竞争组播，行为会微妙很多。
//   Windows 的 DnsServiceRegister 同样是系统级实现。所以按平台走系统 API。
//
// 这一层只负责「把服务公布出去」。发现别的设备（浏览）不在范围内 ——
// 本项目是接收端，只需要被手机看到。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace adisplay::discovery {

// TXT 记录的一条键值对。AirPlay 的很多能力协商都靠它传递，
// 例如 deviceid、features、model、srcvers（文档 3.1.2）。
struct TxtRecord {
    std::string key;
    std::string value;
};

// 要公布的一条服务。
struct MdnsServiceInfo {
    // 服务类型，形如 "_airplay._tcp"（不带末尾的 ".local"）。
    std::string service_type;

    // 实例名，即手机端列表里显示的名字，取自设备名称（文档 2.4）。
    std::string instance_name;

    // 服务端口。
    uint16_t port = 0;

    // TXT 记录。
    std::vector<TxtRecord> txt;

    // 主机名，形如 "adisplay.local"。留空表示由实现取本机主机名。
    std::string host_name;
};

// 发布一条 mDNS 服务。
//
// 一个实例只管一条服务 —— AirPlay 要同时公布 _airplay._tcp 与 _raop._tcp，
// 那就建两个实例。这样改名时逐个重新发布，逻辑比在一个对象里管理多个
// 注册要清楚得多。
class IMdnsPublisher {
public:
    virtual ~IMdnsPublisher() = default;

    IMdnsPublisher(const IMdnsPublisher&) = delete;
    IMdnsPublisher& operator=(const IMdnsPublisher&) = delete;

    // 公布服务。已经公布时先撤回再重新公布（改名走的就是这条路径，
    // 文档 2.4 要求改完即时生效、不重启软件）。
    // 失败时返回 false 并填充 out_error。
    virtual bool publish(const MdnsServiceInfo& info, std::string* out_error) = 0;

    // 撤回公布。未公布时是安全的空操作。
    virtual void withdraw() = 0;

    virtual bool is_published() const = 0;

protected:
    IMdnsPublisher() = default;
};

// 取当前平台的实现。不支持的平台返回 nullptr。
std::unique_ptr<IMdnsPublisher> create_mdns_publisher();

// 当前平台是否支持 mDNS 发布。界面层据此决定是否提示用户。
bool is_mdns_supported();

}  // namespace adisplay::discovery
