// ADisplay —— 网络小工具
//
// 只放跨协议通用的部分。真正的服务（mDNS、SSDP、HTTP）在 core/discovery 与
// protocols/ 下，这里不做任何监听或收发。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adisplay::common {

// 检测 TCP 端口能否绑定。用于启动时排查占用 ——
// 文档 4.5 提到 macOS 12+ 系统自带「AirPlay 接收器」会占住 7000 端口。
//
// 返回 AD 风格的结果码：
//   0  可用
//   5  被占用（对应 adisplay.h 的 AD_ERR_PORT_IN_USE）
//   7  其他网络错误
int probe_tcp_port(uint16_t port);

// 检测 UDP 端口能否绑定（mDNS 5353、SSDP 1900 用）。
int probe_udp_port(uint16_t port);

// 从 start_port 起向上找第一个可用端口，最多试 max_attempts 次。
// 找不到返回 0。
uint16_t find_available_tcp_port(uint16_t start_port, int max_attempts);

struct NetworkAddress {
    std::string interface_name;  // 如 "en0"、"以太网"
    std::string address;         // 点分十进制或 IPv6 字面量
    std::string netmask;
    bool is_ipv6 = false;
    bool is_loopback = false;
    // 虚拟网卡（VMware / VirtualBox / WSL / VPN / Docker 桥）。文档 6.3 提到
    // 多网卡会导致广播发错网段，默认要把这些排除掉。
    bool is_virtual = false;
};

// 枚举本机所有网卡的地址。
std::vector<NetworkAddress> enumerate_network_addresses();

// 只取适合对外广播的地址（跳过回环与虚拟网卡）。
std::vector<NetworkAddress> broadcastable_addresses();

// 取本机在局域网中的 IPv4 地址，没有则返回空串。
// 电视端待机页要显示它（文档 2.3）。
std::string primary_local_ipv4();

// 判断网卡名是否像虚拟网卡。
bool is_virtual_interface(const std::string& interface_name);

// 返回形如 "Darwin/24.6.0"、"Windows/10.0"、"Linux/6.8" 的系统标识，
// 用于 SSDP 的 SERVER 头。UPnP 规范要求这个头带上操作系统，
// 缺了部分客户端会跳过设备。
std::string operating_system_name();

}  // namespace adisplay::common
