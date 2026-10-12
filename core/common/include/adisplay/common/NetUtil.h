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
    // 虚拟网卡（VMware / VirtualBox / WSL / VPN / ZeroTier / Docker 桥）。
    // 文档 6.3 提到多网卡会导致广播发错网段 —— 那件事现在的处理办法是
    // **逐块网卡生成各自的地址**（见 SsdpMessage.h 的 location_for），
    // 所以这里只标出来、不再一律排除：VPN 与覆盖网上的设备也得能被找到，
    // 而「本机地址」那种给人看的字段仍然优先真实网卡（见 primary_local_ipv4）。
    bool is_virtual = false;
};

// 枚举本机所有网卡的地址。
std::vector<NetworkAddress> enumerate_network_addresses();

// 取本机所有可以对外广播的地址：跳过回环，**虚拟网卡也包含在内**
// （VPN / ZeroTier / Tailscale / 虚拟机网桥），每条的 is_virtual 标出它的身份。
//
// 为什么不再排除虚拟网卡：原来的理由是「多网卡会广播到错误网段」，而
// 那个问题已经在 SSDP 里解决了 —— 每块网卡发出去的 LOCATION 用它自己的地址
// （见 SsdpMessage.h 的 location_for）。排除掉的代价则是：整机只接着一条 VPN
// 时一个地址都没有（SSDP 干脆起不来），以及覆盖网上的设备永远搜不到这台机器。
std::vector<NetworkAddress> broadcastable_addresses();

// 按界面上的「广播网卡」设置，筛出真正要广播的地址。
//
// 规则三条，与那行设置的两个选项一一对应：
//   * include_virtual = false（「自动」）：只留真实网卡；
//   * include_virtual = true（「也包含 VPN / 虚拟网卡」）：真实与虚拟都留；
//   * **例外（两种模式下都生效）**：按上面筛完一个都不剩时（典型：整机只挂着
//     一条 VPN），退回「全部可用地址」—— 否则 SSDP 连启动都做不到，用户看到的
//     是「服务开着、桌上搜不到设备」。
// 回环与 IPv6 始终排除（没有意义；UPnP 的组播本来就是 IPv4 的）。
//
// 做成纯函数是为了能被单元测试覆盖：把「哪几块网卡会被广播」从「跑测试那台
// 机器碰巧装了什么网卡」里解耦出来。
std::vector<NetworkAddress> filter_broadcast_addresses(
    const std::vector<NetworkAddress>& entries, bool include_virtual);

// 从一份网卡列表里挑「本机地址」（电视端待机页与日志里显示的那个）。
//
// 规则是**优先真实网卡，都没有才退回虚拟网卡**：这个地址是给人看、给用户照着
// 连的，宁可报出 VPN 的地址（至少说明本机只有 VPN 可用），也不能在两者都在时
// 报一个局域网里连不上的 VPN 地址把用户带偏。
//
// 单独成函数是为了能被单元测试直接覆盖（构造出来的列表，不依赖跑测试的机器
// 真实装了什么网卡）。
std::string pick_primary_ipv4(const std::vector<NetworkAddress>& entries);

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
