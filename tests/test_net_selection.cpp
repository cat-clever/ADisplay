// 本机地址选择的测试。
//
// 这一层看着只是「挑个 IP」，但两种错法都很隐蔽：
//   * 挑到虚拟网卡（VPN / ZeroTier）的地址 → 界面上显示一个局域网里连不上的地址，
//     用户照着它排查只会更迷惑；
//   * 反过来把虚拟网卡一律排除 → 整机只有一条 VPN 时一个地址都说不出来。
//
// 所以把「优先真实网卡，没有才退回虚拟网卡」这条规则用**构造出来的**网卡列表
// 钉死：不依赖跑测试的机器真实装了什么网卡，CI 与开发机结果一致。
#include <adisplay/common/NetUtil.h>

#include "AdTest.h"

#include <string>
#include <vector>

using namespace adisplay::common;

namespace {

NetworkAddress make(const std::string& name, const std::string& address,
                    bool is_virtual = false, bool is_ipv6 = false) {
    NetworkAddress entry;
    entry.interface_name = name;
    entry.address = address;
    entry.is_virtual = is_virtual;
    entry.is_ipv6 = is_ipv6;
    return entry;
}

}  // namespace

AD_TEST(net_selection_primary, "本机地址：优先真实网卡，没有才用虚拟网卡") {
    // 虚拟网卡排在前面也不能赢 —— 枚举顺序由系统决定，不能让它决定结果。
    AD_CHECK_EQ(pick_primary_ipv4({
                    make("ztxxxxxxxx", "172.22.1.5", true),
                    make("以太网", "192.168.1.20"),
                }),
                std::string("192.168.1.20"));

    // 整机只有 VPN / 覆盖网：这时要给得出地址，否则界面上一片空白。
    AD_CHECK_EQ(pick_primary_ipv4({make("utun3", "10.8.0.2", true)}),
                std::string("10.8.0.2"));

    // 只报 IPv4：IPv6 地址不能当「本机地址」给用户。
    AD_CHECK_EQ(pick_primary_ipv4({make("en0", "fe80::1", false, true)}),
                std::string());

    // 一个能用的都没有：返回空串，而不是随便挑一个。
    AD_CHECK_EQ(pick_primary_ipv4({}), std::string());
}

AD_TEST(net_selection_virtual_names, "虚拟网卡按名字识别（含 VPN 与覆盖网）") {
    // macOS 的隧道 / 点对点、Linux 的隧道、Windows 上常见的虚拟适配器名，
    // 以及这次要试的 ZeroTier / Tailscale / WireGuard。
    AD_CHECK(is_virtual_interface("utun3"));
    AD_CHECK(is_virtual_interface("awdl0"));
    AD_CHECK(is_virtual_interface("tun0"));
    AD_CHECK(is_virtual_interface("tap0"));
    AD_CHECK(is_virtual_interface("wg0"));
    AD_CHECK(is_virtual_interface("ztxxxxxxxx"));
    AD_CHECK(is_virtual_interface("tailscale0"));
    AD_CHECK(is_virtual_interface("ZeroTier One [xxxxxxxx]"));
    AD_CHECK(is_virtual_interface("Tailscale"));
    AD_CHECK(is_virtual_interface("TAP-Windows Adapter V9"));
    AD_CHECK(is_virtual_interface("vEthernet (WSL)"));

    // 真实网卡名不能被误判 —— 误判的表现是「只接一条网线的机器一个地址都说不出」。
    AD_CHECK(!is_virtual_interface("en0"));
    AD_CHECK(!is_virtual_interface("eth0"));
    AD_CHECK(!is_virtual_interface("WLAN"));
    AD_CHECK(!is_virtual_interface("以太网"));
    AD_CHECK(!is_virtual_interface(""));
}

int main() {
    return adtest::run_all("网卡与地址选择测试");
}
