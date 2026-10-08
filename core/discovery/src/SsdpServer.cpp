// ADisplay —— SSDP 服务端实现
//
// 一份代码跨平台：socket 的差异集中在下面几个小包装里，
// 协议逻辑本身与平台无关。
#include <adisplay/discovery/SsdpServer.h>

#include <adisplay/common/Log.h>
#include <adisplay/common/Random.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>   // time / tm / strftime，SSDP 的 DATE 头要 RFC 1123 格式
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

// SSDP 的固定组播地址与端口。
constexpr const char* kSsdpMulticastAddress = "239.255.255.250";
constexpr uint16_t kSsdpPort = 1900;

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

std::string to_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trim(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// 从 HTTP 风格的报文里取某个头。SSDP 的头名大小写不敏感。
std::string header_value(const std::string& message, const std::string& name) {
    const std::string lower_name = to_lower(name);
    std::istringstream stream(message);
    std::string line;
    bool first_line = true;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (first_line) {
            first_line = false;
            continue;   // 跳过请求行 / 状态行
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (to_lower(line.substr(0, colon)) == lower_name) {
            return trim(line.substr(colon + 1));
        }
    }
    return std::string();
}

// RFC 1123 格式的日期，SSDP 的 DATE 头要求这个格式。
std::string http_date() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    ::gmtime_s(&utc, &now);
#else
    ::gmtime_r(&now, &utc);
#endif
    char buffer[64] = {0};
    std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &utc);
    return std::string(buffer);
}

// 一条通告对应一个 (NT, USN) 组合。rootdevice、uuid、devicetype
// 以及每个 service type 都要各发一条。
struct NotificationTarget {
    std::string nt;
    std::string usn;
};

std::vector<NotificationTarget> build_targets(const SsdpAdvertisement& ad) {
    std::vector<NotificationTarget> targets;

    const auto append = [&targets, &ad](const std::string& nt) {
        targets.push_back(NotificationTarget{nt, ad.udn + "::" + nt});
    };

    append("upnp:rootdevice");
    append(ad.udn);
    append(ad.device_type);
    for (const std::string& service : ad.service_types) {
        append(service);
    }
    return targets;
}

std::string build_alive_message(const SsdpAdvertisement& ad, const NotificationTarget& target) {
    std::ostringstream out;
    out << "NOTIFY * HTTP/1.1\r\n"
        << "HOST: " << kSsdpMulticastAddress << ":" << kSsdpPort << "\r\n"
        << "CACHE-CONTROL: max-age=" << ad.max_age_seconds << "\r\n"
        << "LOCATION: " << ad.location << "\r\n"
        << "NT: " << target.nt << "\r\n"
        << "NTS: ssdp:alive\r\n"
        << "SERVER: " << ad.server_header << "\r\n"
        << "USN: " << target.usn << "\r\n"
        << "\r\n";
    return out.str();
}

// byebye 不带 LOCATION 与 CACHE-CONTROL —— 规范里这个报文只用于注销。
std::string build_byebye_message(const SsdpAdvertisement& ad, const NotificationTarget& target) {
    std::ostringstream out;
    out << "NOTIFY * HTTP/1.1\r\n"
        << "HOST: " << kSsdpMulticastAddress << ":" << kSsdpPort << "\r\n"
        << "NT: " << target.nt << "\r\n"
        << "NTS: ssdp:byebye\r\n"
        << "USN: " << target.usn << "\r\n"
        << "\r\n";
    return out.str();
}

// 回复 M-SEARCH。必须单播回请求方 —— 用组播回复会让局域网里每台设备
// 都收到一堆不属于自己的响应，部分手机端会因此显示重复设备。
std::string build_search_response(const SsdpAdvertisement& ad, const std::string& st) {
    std::ostringstream out;
    out << "HTTP/1.1 200 OK\r\n"
        << "CACHE-CONTROL: max-age=" << ad.max_age_seconds << "\r\n"
        << "DATE: " << http_date() << "\r\n"
        // EXT 头是规范要求的，值必须为空。
        << "EXT:\r\n"
        << "LOCATION: " << ad.location << "\r\n"
        << "SERVER: " << ad.server_header << "\r\n"
        << "ST: " << st << "\r\n"
        << "USN: " << ad.udn << "::" << st << "\r\n"
        << "\r\n";
    return out.str();
}

// 判断查询的 ST 是否指向我们。ssdp:all 表示「把所有设备都报一遍」。
bool matches_search_target(const SsdpAdvertisement& ad, const std::string& st) {
    if (st.empty()) {
        return false;
    }
    if (st == "ssdp:all") {
        return true;
    }
    if (st == "upnp:rootdevice" || st == ad.udn || st == ad.device_type) {
        return true;
    }
    for (const std::string& service : ad.service_types) {
        if (st == service) {
            return true;
        }
    }
    return false;
}

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
        local.sin_port = htons(kSsdpPort);

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

        // 加入组播组，否则收不到发往 239.255.255.250 的 M-SEARCH。
        ip_mreq membership;
        std::memset(&membership, 0, sizeof(membership));
        membership.imr_multiaddr.s_addr = ::inet_addr(kSsdpMulticastAddress);
        membership.imr_interface.s_addr = htonl(INADDR_ANY);

#if defined(_WIN32)
        ::setsockopt(socket_handle, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     reinterpret_cast<const char*>(&membership), sizeof(membership));
#else
        ::setsockopt(socket_handle, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     &membership, sizeof(membership));
#endif

        // 关掉组播回环没有意义（本机也要能看到自己发出去的包，便于排障），
        // 但要保证发送接口由系统按路由选择，不能固定到某块网卡 ——
        // 文档 6.3 提到多网卡与虚拟网卡会让广播发错网段。
        unsigned char ttl = 4;
#if defined(_WIN32)
        ::setsockopt(socket_handle, IPPROTO_IP, IP_MULTICAST_TTL,
                     reinterpret_cast<const char*>(&ttl), sizeof(ttl));
#else
        ::setsockopt(socket_handle, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
#endif

        return true;
    }

    // ---- 发送 --------------------------------------------------------------

    void send_to(const std::string& data, const sockaddr_in& target) {
        if (socket_handle == kInvalidSocket) {
            return;
        }
        ::sendto(socket_handle, data.data(), static_cast<int>(data.size()), 0,
                 reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    }

    void send_multicast(const std::string& data) {
        sockaddr_in target;
        std::memset(&target, 0, sizeof(target));
        target.sin_family = AF_INET;
        target.sin_addr.s_addr = ::inet_addr(kSsdpMulticastAddress);
        target.sin_port = htons(kSsdpPort);
        send_to(data, target);
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
        for (const NotificationTarget& target : build_targets(snapshot)) {
            send_multicast(build_alive_message(snapshot, target));
        }
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
        for (const NotificationTarget& target : build_targets(snapshot)) {
            send_multicast(build_byebye_message(snapshot, target));
        }
    }

    // ---- 接收 --------------------------------------------------------------

    void handle_datagram(const char* data, int length, const sockaddr_in& sender) {
        const std::string message(data, static_cast<std::size_t>(length));

        // 只处理 M-SEARCH。NOTIFY 是别的设备在宣告，接收端不需要理会。
        if (message.compare(0, 8, "M-SEARCH") != 0) {
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

        send_to(build_search_response(snapshot, st), sender);
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
