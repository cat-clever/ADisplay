// ADisplay —— SSDP 服务端（UPnP 设备发现）
//
// 文档 3.2：DLNA 的角色是 DMR（Digital Media Renderer），
// 需要「SSDP 组播应答 M-SEARCH，并周期性发送 NOTIFY alive」。
//
// 与 mDNS 不同，SSDP 没有可用的系统服务，这里是自己实现的一份，
// 一份代码跨平台。
//
// 协议要点（UDP，组播地址 239.255.255.250:1900）：
//   * 手机发 M-SEARCH 组播查询，我们要**单播**回复 HTTP/1.1 200 OK。
//   * 我们要周期性组播 NOTIFY ssdp:alive 宣告自己还活着，
//     间隔取 max-age 的一半，否则手机会认为设备掉线。
//   * 关闭时组播 NOTIFY ssdp:byebye，让手机立刻把设备移出列表 ——
//     少了这一步，设备要等到 max-age 过期才消失。
//
// 注意：M-SEARCH 的回复必须单播回请求方。用组播回复的话，
// 局域网里每台设备都会收到一堆不属于自己的响应，部分手机端会因此
// 显示重复设备或直接忽略。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace adisplay::discovery {

// 一次 SSDP 宣告要公布的全部内容。
struct SsdpAdvertisement {
    // DLNA 的 UDN，形如 "uuid:xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"。
    // 取自 DeviceIdentity 的 uuid，改名不影响它（文档 2.4「标识不变」）。
    std::string udn;

    // 设备类型，MediaRenderer 固定为
    // "urn:schemas-upnp-org:device:MediaRenderer:1"。
    std::string device_type;

    // 设备描述 XML 的完整 URL，形如
    // "http://192.168.1.10:49152/description.xml"。
    // 手机拿到它之后会去 GET 这个地址取设备详情。
    std::string location;

    // 各服务的类型，会逐个作为单独的 NT 通告出去：
    //   urn:schemas-upnp-org:service:AVTransport:1
    //   urn:schemas-upnp-org:service:RenderingControl:1
    //   urn:schemas-upnp-org:service:ConnectionManager:1
    std::vector<std::string> service_types;

    // 宣告有效期（秒）。文档与 UPnP 惯例是 1800。
    // 我们会按它的一半周期性地重发 alive。
    int max_age_seconds = 1800;

    // SERVER 头的内容，形如 "Windows/10.0 UPnP/1.0 ADisplay/0.1.0"。
    // 部分手机端会检查 UPnP/1.0 这个标记，缺了可能不认。
    std::string server_header;
};

class SsdpServer {
public:
    SsdpServer();
    ~SsdpServer();

    SsdpServer(const SsdpServer&) = delete;
    SsdpServer& operator=(const SsdpServer&) = delete;

    // 绑定 1900 端口、加入组播组，并发出第一轮 alive。
    //
    // 失败原因常见的是 1900 被别的 UPnP 服务（Windows 的 SSDP 服务、
    // 其他投屏软件）占着。此时返回 false 并填充 out_error。
    bool start(const SsdpAdvertisement& advertisement, std::string* out_error);

    // 发 byebye 并停止。
    void stop();

    // 运行中更新宣告内容（设备改名、IP 变化、端口变化时用）。
    // 会先发一轮 byebye 再重新 alive，让手机端干净地刷新。
    bool update(const SsdpAdvertisement& advertisement, std::string* out_error);

    bool is_running() const;

    // 已回复过的 M-SEARCH 数量。排障用 —— 手机上搜不到设备时，
    // 这个数字能区分「根本没收到查询」（网络/防火墙问题）和
    // 「收到了但回复没被接受」（报文内容问题）。
    uint64_t responded_search_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace adisplay::discovery
