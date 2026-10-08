// ADisplay —— SSDP 服务端实现
//
// 一份代码跨平台：socket 的差异集中在下面几个小包装里，
// 协议逻辑本身与平台无关。
#include <adisplay/discovery/SsdpServer.h>

#include "SsdpMessage.h"

#include <adisplay/common/Log.h>
#include <adisplay/common/NetUtil.h>
#include <adisplay/common/Random.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

namespace adisplay::discovery {
namespace {

// 报文的构建与解析都在 SsdpMessage.h/.cpp 里 —— 那些是纯函数，
// 单独放是为了能被单元测试直接覆盖。这里只留收发与线程。
using namespace ssdp;

// 收到 M-SEARCH 后最多等这么久再回复。UPnP 规范建议在 0..MX 秒内随机延迟，
// 避免局域网内所有设备同时响应造成拥塞。局域网里设备很少，
// 取一个小值即可 —— 太大反而让手机等得久。
constexpr int kMaxResponseDelayMs = 120;

// 事件循环每次 select 的超时，决定停止请求的响应速度。
constexpr int kSelectTimeoutUsec = 200 * 1000;

// 向某地址发送后，同一个 UDP socket 也要能收到单播回复，
// 所以不做 connect()，每次都用 sendto。
constexpr int kMaxDatagramSize = 2048;

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

void close_socket(SocketHandle handle) {
    if (handle == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

// Windows 上 socket 库要先初始化。跨平台编译时这里是个空壳。
class SocketRuntime {
public:
    SocketRuntime() {
#if defined(_WIN32)
        WSADATA data;
        ok_ = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
    }
    ~SocketRuntime() {
#if defined(_WIN32)
        if (ok_) {
            ::WSACleanup();
        }
#endif
    }
    bool ok() const {
#if defined(_WIN32)
        return ok_;
#else
        return true;
#endif
    }

private:
#if defined(_WIN32)
    bool ok_ = false;
#endif
};

}  // namespace

// ===========================================================================

struct SsdpServer::Impl {
    SocketRuntime runtime;

    SocketHandle socket_handle = kInvalidSocket;
    std::thread worker;
    std::atomic<bool> running{false};
    mutable std::mutex mutex;
    SsdpAdvertisement advertisement;
    std::atomic<uint64_t> responded_count{0};
    std::chrono::steady_clock::time_point last_announce;

    // 实际用于收发广播的网卡。可能有多块 —— 详见 open_socket 里的说明。
    struct InterfaceBinding {
        std::string name;
        std::string address;
        // 掩码用于判断某个请求方是否与这块网卡同网段。
        // 为空时按 /24 兜底（Windows 的枚举分支目前不填掩码）。
        std::string netmask;
    };
    std::vector<InterfaceBinding> interfaces_;

    // ---- 组播相关的 socket 设置 -------------------------------------------

    bool open_socket(std::string* out_error) {
        socket_handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_handle == kInvalidSocket) {
            set_error(out_error, "无法创建 UDP socket");
            return false;
        }

        // 允许 SO_REUSEADDR：局域网上可能已有别的 UPnP 服务（Windows 的
        // SSDP 服务、其他投屏软件）绑着 1900，不能因为它们在跑就整个失败。
        int reuse = 1;
#if defined(_WIN32)
        ::setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
        ::setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

        sockaddr_in local;
        std::memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(kPort);

        if (::bind(socket_handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            const std::string message =
                "SSDP 端口 1900 绑定失败。可能被别的 UPnP 服务占用"
                "（Windows 上常见于「SSDP 发现」服务或其它投屏软件）。";
            set_error(out_error, message);
            AD_LOG_ERROR("{}", message);
            close_socket(socket_handle);
            socket_handle = kInvalidSocket;
            return false;
        }

        // ---- 选定要参与广播的网卡 ----------------------------------------
        //
        // 这里是按「所有可用网卡都发」做的，不是只挑一块。
        //
        // 原因：真实机器上经常同时挂着多个网段 —— 有线一个、Wi-Fi 一个，
        // 还可能是手机热点（172.20.10.x）与网络共享（192.168.137.x）并存。
        // 手机连的是哪个网段我们事先无从知道，只挑一块就有一半概率挑错，
        // 表现是「手机上搜不到设备」，而且没有任何报错。
        //
        // 加入组播组可以对多块网卡分别做；发送时每次切换 IP_MULTICAST_IF
        // 即可。这样不管手机在哪个网段都能收到。
        interfaces_.clear();
        for (const common::NetworkAddress& candidate : common::broadcastable_addresses()) {
            if (candidate.is_ipv6) {
                continue;   // UPnP 的组播地址是 IPv4 的
            }
            InterfaceBinding binding;
            binding.name = candidate.interface_name;
            binding.address = candidate.address;
            binding.netmask = candidate.netmask;
            interfaces_.push_back(binding);
        }

        if (interfaces_.empty()) {
            set_error(out_error,
                      "找不到可用的局域网网卡。请确认已连接 Wi-Fi 或网线。");
            close_socket(socket_handle);
            socket_handle = kInvalidSocket;
            return false;
        }

        {
            std::string joined;
            for (const InterfaceBinding& binding : interfaces_) {
                if (!joined.empty()) {
                    joined += "，";
                }
                joined += binding.name + "（" + binding.address + "）";
            }
            AD_LOG_INFO("SSDP 将在 {} 块网卡上广播：{}",
                        interfaces_.size(), joined);
        }

        // ---- 逐块网卡加入组播组 ------------------------------------------
        //
        // 返回值必须检查：失败的那块网卡收不到任何组播，如果全都失败，
        // 表现就是「手机搜不到设备」，而且不会有任何报错 ——
        // 排查时会误以为是手机或路由器的问题。
        std::size_t joined_count = 0;
        std::string last_join_error;

        for (const InterfaceBinding& binding : interfaces_) {
            ip_mreq membership;
            std::memset(&membership, 0, sizeof(membership));
            membership.imr_multiaddr.s_addr = ::inet_addr(kMulticastAddress);
            membership.imr_interface.s_addr = ::inet_addr(binding.address.c_str());

            std::string join_error;
            if (set_socket_option(IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                  &membership, sizeof(membership),
                                  "IP_ADD_MEMBERSHIP", &join_error)) {
                ++joined_count;
            } else {
                // 单块失败不影响其他网卡 —— 继续尝试，最后再判断。
                last_join_error = binding.name + "：" + join_error;
                AD_LOG_WARN("网卡 {} 加入组播组失败，跳过：{}",
                            binding.name, join_error);
            }
        }

        if (joined_count == 0) {
            set_error(out_error,
                      "所有网卡都无法加入 SSDP 组播组。" + last_join_error);
            close_socket(socket_handle);
            socket_handle = kInvalidSocket;
            return false;
        }

        /* 组播回环保持默认开启：本机也要能看到自己发出去的包，
           排障时很有用，而且不影响其他设备。 */
        unsigned char ttl = 4;
        if (!set_socket_option(IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl),
                               "IP_MULTICAST_TTL", out_error)) {
            close_socket(socket_handle);
            socket_handle = kInvalidSocket;
            return false;
        }

        return true;
    }

    // 统一的 setsockopt 包装：失败时把 errno / WSA 错误码一起报出来。
    // 这类失败在原来的代码里是静默的，导致功能坏掉却毫无线索。
    bool set_socket_option(int level, int option, const void* value, std::size_t size,
                           const char* option_name, std::string* out_error) {
#if defined(_WIN32)
        const int result = ::setsockopt(socket_handle, level, option,
                                        static_cast<const char*>(value),
                                        static_cast<int>(size));
#else
        const int result = ::setsockopt(socket_handle, level, option, value, size);
#endif
        if (result == 0) {
            return true;
        }

#if defined(_WIN32)
        const int code = ::WSAGetLastError();
#else
        const int code = errno;
#endif
        const std::string message =
            std::string("设置 socket 选项 ") + option_name + " 失败（错误码 " +
            std::to_string(code) + "）";
        AD_LOG_ERROR("{}", message);
        set_error(out_error, message);
        return false;
    }


    // ---- 发送 --------------------------------------------------------------

    void send_to(const std::string& data, const sockaddr_in& target) {
        if (socket_handle == kInvalidSocket) {
            return;
        }
        ::sendto(socket_handle, data.data(), static_cast<int>(data.size()), 0,
                 reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    }

    // 逐块网卡发送组播。
    //
    // builder 接收「这块网卡的地址」，返回要发出去的报文 —— 报文内容会
    // 因网卡而异，因为里面的 LOCATION 必须用该网卡自己的地址。
    //
    // 一个 socket 只能设一个组播出接口，所以要发一轮、换一次接口。
    // 不这么做的话，只有默认路由那块网卡上的设备能收到。
    void send_multicast_per_interface(
        const std::function<std::string(const std::string& address)>& builder) {
        sockaddr_in target;
        std::memset(&target, 0, sizeof(target));
        target.sin_family = AF_INET;
        target.sin_addr.s_addr = ::inet_addr(kMulticastAddress);
        target.sin_port = htons(kPort);

        for (const InterfaceBinding& binding : interfaces_) {
            in_addr outbound;
            outbound.s_addr = ::inet_addr(binding.address.c_str());

            // 切换失败就跳过这块网卡：宁可少发一块，也不要因为一次失败
            // 让整轮广播中断。
            if (!set_socket_option(IPPROTO_IP, IP_MULTICAST_IF,
                                   &outbound, sizeof(outbound),
                                   "IP_MULTICAST_IF", nullptr)) {
                continue;
            }
            send_to(builder(binding.address), target);
        }
    }

    // 判断两个 IPv4 地址是否在同一网段。
    static bool same_subnet(const std::string& a, const std::string& b,
                            const std::string& netmask) {
        const in_addr addr_a = {::inet_addr(a.c_str())};
        const in_addr addr_b = {::inet_addr(b.c_str())};
        if (addr_a.s_addr == INADDR_NONE || addr_b.s_addr == INADDR_NONE) {
            return false;
        }
        // 掩码缺失时按 /24 兜底：绝大多数家用网络是这个。
        const in_addr mask = {netmask.empty() ? ::inet_addr("255.255.255.0")
                                              : ::inet_addr(netmask.c_str())};
        if (mask.s_addr == INADDR_NONE) {
            return false;
        }
        return (addr_a.s_addr & mask.s_addr) == (addr_b.s_addr & mask.s_addr);
    }

    // 找出与某个对端同网段的那块网卡。
    //
    // 用于回复 M-SEARCH：响应里的 LOCATION 必须是「对端能访问到的我们的
    // 地址」。用错网段的地址，手机会拿到一个连不上的 URL，
    // 表现是搜到了设备但点不动、或者设备一闪而过。
    const InterfaceBinding* interface_for_peer(const std::string& peer_address) const {
        for (const InterfaceBinding& binding : interfaces_) {
            if (same_subnet(binding.address, peer_address, binding.netmask)) {
                return &binding;
            }
        }
        // 匹配不上时用第一块 —— 总比不回复强。
        return interfaces_.empty() ? nullptr : &interfaces_.front();
    }

    void announce_alive() {
        SsdpAdvertisement snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = advertisement;
        }
        if (snapshot.location.empty()) {
            return;
        }
        const std::vector<NotificationTarget> targets = build_targets(snapshot);

        // 逐块网卡发，且每块网卡用各自的 LOCATION —— 手机从哪个网段收到，
        // 就用哪个网段的地址去拉设备描述。
        send_multicast_per_interface([&snapshot, &targets](const std::string& address) {
            std::string payload;
            const std::string location = location_for(snapshot, address);
            for (const NotificationTarget& target : targets) {
                payload += build_alive_message(snapshot, target, location);
            }
            return payload;
        });

        last_announce = std::chrono::steady_clock::now();
    }

    void announce_byebye() {
        SsdpAdvertisement snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = advertisement;
        }
        if (snapshot.location.empty()) {
            return;
        }
        // byebye 不带 LOCATION，所以逐块网卡的报文是一样的。
        std::string payload;
        for (const NotificationTarget& target : build_targets(snapshot)) {
            payload += build_byebye_message(snapshot, target);
        }
        send_multicast_per_interface([&payload](const std::string&) { return payload; });
    }

    // ---- 接收 --------------------------------------------------------------

    void handle_datagram(const char* data, int length, const sockaddr_in& sender) {
        const std::string message(data, static_cast<std::size_t>(length));

        // 只处理 M-SEARCH。NOTIFY 是别的设备在宣告，接收端不需要理会。
        if (!is_search_request(message)) {
            return;
        }

        const std::string st = header_value(message, "ST");
        if (st.empty()) {
            return;
        }

        SsdpAdvertisement snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = advertisement;
        }
        if (snapshot.location.empty() || !matches_search_target(snapshot, st)) {
            return;
        }

        // UPnP 建议在 0..MX 秒内随机延迟后回复，避免所有设备同时响应。
        const int delay_ms = static_cast<int>(common::random_below(kMaxResponseDelayMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

        // 用与请求方同网段的那块网卡的地址拼 LOCATION。
        char peer_text[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &sender.sin_addr, peer_text, sizeof(peer_text));
        const InterfaceBinding* binding = interface_for_peer(peer_text);
        const std::string address = (binding != nullptr) ? binding->address : std::string();

        send_to(build_search_response(snapshot, st, location_for(snapshot, address)), sender);
        responded_count.fetch_add(1, std::memory_order_relaxed);

        AD_LOG_DEBUG("已回复 M-SEARCH（ST={}）", st);
    }

    void run_loop() {
        while (running.load(std::memory_order_relaxed)) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(socket_handle, &read_set);

            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = kSelectTimeoutUsec;

            const int ready = ::select(static_cast<int>(socket_handle) + 1,
                                       &read_set, nullptr, nullptr, &timeout);

            if (!running.load(std::memory_order_relaxed)) {
                return;
            }

            if (ready > 0 && FD_ISSET(socket_handle, &read_set)) {
                char buffer[kMaxDatagramSize];
                sockaddr_in sender;
                std::memset(&sender, 0, sizeof(sender));
#if defined(_WIN32)
                int sender_length = sizeof(sender);
#else
                socklen_t sender_length = sizeof(sender);
#endif
                const int received = ::recvfrom(
                    socket_handle, buffer, sizeof(buffer) - 1, 0,
                    reinterpret_cast<sockaddr*>(&sender), &sender_length);

                if (received > 0) {
                    buffer[received] = '\0';
                    handle_datagram(buffer, received, sender);
                }
            }

            // 周期性重发 alive。间隔取 max-age 的一半 —— 这是 UPnP 的惯例，
            // 留足余量，免得丢一个包就让手机认为设备掉线。
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_announce).count();

            int interval_seconds = 900;
            {
                std::lock_guard<std::mutex> lock(mutex);
                interval_seconds = std::max(60, advertisement.max_age_seconds / 2);
            }

            if (elapsed >= interval_seconds) {
                announce_alive();
            }
        }
    }

    void set_error(std::string* out_error, const std::string& message) {
        if (out_error != nullptr) {
            *out_error = message;
        }
    }
};

// ===========================================================================

SsdpServer::SsdpServer() : impl_(new Impl()) {}

SsdpServer::~SsdpServer() {
    stop();
}

bool SsdpServer::start(const SsdpAdvertisement& advertisement, std::string* out_error) {
    if (impl_->running.load()) {
        return update(advertisement, out_error);
    }

    if (advertisement.udn.empty() || advertisement.location.empty()) {
        if (out_error != nullptr) {
            *out_error = "SSDP 通告缺少 UDN 或 LOCATION";
        }
        return false;
    }

    if (!impl_->runtime.ok()) {
        if (out_error != nullptr) {
            *out_error = "网络子系统初始化失败";
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->advertisement = advertisement;
    }

    if (!impl_->open_socket(out_error)) {
        return false;
    }

    impl_->running.store(true);
    impl_->worker = std::thread([this]() { impl_->run_loop(); });

    // 启动后立刻宣告一轮。不宣告的话手机要等到自己发 M-SEARCH 才能发现我们，
    // 而有些手机端只在进入投屏页面时才搜索一次。
    impl_->announce_alive();

    AD_LOG_INFO("SSDP 已启动，LOCATION={}", advertisement.location);
    return true;
}

void SsdpServer::stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }

    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    // 先发 byebye 再关 socket —— 顺序反了就发不出去了。
    // 少了这一步，手机要等到 max-age 过期才会把设备移出列表。
    impl_->announce_byebye();

    close_socket(impl_->socket_handle);
    impl_->socket_handle = kInvalidSocket;

    AD_LOG_INFO("SSDP 已停止");
}

bool SsdpServer::update(const SsdpAdvertisement& advertisement, std::string* out_error) {
    if (advertisement.udn.empty() || advertisement.location.empty()) {
        if (out_error != nullptr) {
            *out_error = "SSDP 通告缺少 UDN 或 LOCATION";
        }
        return false;
    }

    if (!impl_->running.load()) {
        return start(advertisement, out_error);
    }

    // 先让手机端忘掉旧的，再宣告新的，避免列表里同时出现新旧两条。
    impl_->announce_byebye();

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->advertisement = advertisement;
    }

    impl_->announce_alive();
    return true;
}

bool SsdpServer::is_running() const {
    return impl_->running.load();
}

uint64_t SsdpServer::responded_search_count() const {
    return impl_->responded_count.load(std::memory_order_relaxed);
}

}  // namespace adisplay::discovery
