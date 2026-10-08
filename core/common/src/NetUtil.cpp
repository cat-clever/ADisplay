#include <adisplay/common/NetUtil.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwchar>   // wcslen，把适配器描述从宽字符转 UTF-8 时要用
#include <mutex>
#include <vector>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "iphlpapi.lib")
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/utsname.h>   // uname，拼 SSDP 的 SERVER 头
#  include <unistd.h>
#endif

namespace adisplay::common {
namespace {

#if defined(_WIN32)
// Winsock 必须在使用前初始化一次。用引用计数保证正确的初始化/清理顺序。
class WinsockGuard {
public:
    WinsockGuard() {
        std::lock_guard<std::mutex> lock(mutex());
        if (count() == 0) {
            WSADATA data;
            ok_ = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
        }
        if (ok_) {
            ++count();
        }
    }
    ~WinsockGuard() {
        std::lock_guard<std::mutex> lock(mutex());
        if (ok_ && count() > 0) {
            if (--count() == 0) {
                ::WSACleanup();
            }
        }
    }
    bool ok() const { return ok_; }

private:
    static std::mutex& mutex() {
        static std::mutex instance;
        return instance;
    }
    static int& count() {
        static int instance = 0;
        return instance;
    }
    bool ok_ = false;
};
#else
class WinsockGuard {
public:
    bool ok() const { return true; }
};
#endif

void close_socket(int fd) {
    if (fd < 0) {
        return;
    }
#if defined(_WIN32)
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

int last_socket_error() {
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

bool is_address_in_use(int error_code) {
#if defined(_WIN32)
    return error_code == WSAEADDRINUSE;
#else
    return error_code == EADDRINUSE;
#endif
}

// 判定错误的「端口被占用」含义，映射到 adisplay.h 的返回码。
int classify_bind_error(int error_code) {
    if (is_address_in_use(error_code)) {
        return 5;  // AD_ERR_PORT_IN_USE
    }
#if !defined(_WIN32)
    if (error_code == EACCES) {
        return 6;  // AD_ERR_PERMISSION_DENIED（1024 以下端口需要特权）
    }
#endif
    return 7;  // AD_ERR_NETWORK
}

int probe_port(uint16_t port, int socket_type, int protocol) {
    if (port == 0) {
        return 1;  // AD_ERR_INVALID_ARG
    }

    WinsockGuard guard;
    if (!guard.ok()) {
        return 7;
    }

    const int fd = static_cast<int>(::socket(AF_INET, socket_type, protocol));
#if defined(_WIN32)
    if (fd == INVALID_SOCKET) {
        return 7;
    }
#else
    if (fd < 0) {
        return 7;
    }
#endif

    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    // 刻意不设 SO_REUSEADDR：设了以后在 Linux/macOS 上会让「已处于 TIME_WAIT」
    // 的端口也绑定成功，掩盖真实的占用情况。这里要的是最严格的判定。
    const int bind_result =
        ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));

    if (bind_result != 0) {
        const int error_code = last_socket_error();
        close_socket(fd);
        return classify_bind_error(error_code);
    }

    close_socket(fd);
    return 0;
}

bool starts_with(const std::string& text, const char* prefix) {
    const std::size_t length = std::strlen(prefix);
    return text.size() >= length && text.compare(0, length, prefix) == 0;
}

bool contains_ci(const std::string& haystack, const char* needle) {
    if (needle == nullptr || needle[0] == '\0') {
        return false;
    }
    std::string lower_haystack = haystack;
    std::string lower_needle = needle;
    std::transform(lower_haystack.begin(), lower_haystack.end(), lower_haystack.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(lower_needle.begin(), lower_needle.end(), lower_needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower_haystack.find(lower_needle) != std::string::npos;
}

}  // namespace

std::string operating_system_name() {
#if defined(_WIN32)
    // 不用 RtlGetVersion：那需要 winternl.h 里的 RTL_OSVERSIONINFOW，
    // 而目标平台是 Windows 10 / 11，写死版本号反而更省事也更可靠 ——
    // 这个头只用于让客户端满意 SERVER 头的格式，不影响功能。
    return "Windows/10.0";
#elif defined(__APPLE__)
    // uname 的 release 是 Darwin 内核版本，与 macOS 版本号不同，
    // 但 UPnP 的 SERVER 头里用它更常见（Upnp/1.0 时代的惯例）。
    struct utsname info;
    if (::uname(&info) == 0 && info.release[0] != '\0') {
        return std::string("Darwin/") + info.release;
    }
    return "Darwin";
#else
    struct utsname info;
    if (::uname(&info) == 0 && info.release[0] != '\0') {
        return std::string("Linux/") + info.release;
    }
    return "Linux";
#endif
}

bool is_virtual_interface(const std::string& interface_name) {
    if (interface_name.empty()) {
        return false;
    }

    // 文档 6.3：「多网卡导致广播错误 —— 虚拟网卡（VMware、WSL、VPN）」，
    // 默认要忽略它们，否则 mDNS / SSDP 会广播到错误的网段。
    static const char* kVirtualPrefixes[] = {
        // macOS / BSD
        "utun", "awdl", "llw", "bridge", "gif", "stf", "anpi", "ap1", "XHC",
        // Linux
        "docker", "veth", "br-", "virbr", "vmnet", "vboxnet", "tun", "tap",
        "wg", "zt", "tailscale", "dummy", "bond",
        // Windows（适配器名里常见）
        "vethernet", "hyper-v", "vmware", "virtualbox", "loopback", "tap-",
        "wsl", "npcap", "bluetooth",
    };
    for (const char* prefix : kVirtualPrefixes) {
        if (starts_with(interface_name, prefix)) {
            return true;
        }
    }

    static const char* kVirtualSubstrings[] = {
        "vmware", "virtualbox", "hyper-v", "vethernet", "loopback",
        "virtual", "wsl", "zerotier", "tailscale", "wireguard", "tap-windows",
    };
    for (const char* needle : kVirtualSubstrings) {
        if (contains_ci(interface_name, needle)) {
            return true;
        }
    }

    return false;
}

int probe_tcp_port(uint16_t port) {
    return probe_port(port, SOCK_STREAM,
#if defined(_WIN32)
                      IPPROTO_TCP
#else
                      0
#endif
    );
}

int probe_udp_port(uint16_t port) {
    return probe_port(port, SOCK_DGRAM,
#if defined(_WIN32)
                      IPPROTO_UDP
#else
                      0
#endif
    );
}

uint16_t find_available_tcp_port(uint16_t start_port, int max_attempts) {
    if (max_attempts <= 0) {
        max_attempts = 1;
    }
    // 从 start_port 往上找，注意别越过 65535 回绕成 0。
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        const int candidate = static_cast<int>(start_port) + attempt;
        if (candidate > 65535) {
            break;
        }
        const uint16_t port = static_cast<uint16_t>(candidate);
        if (probe_tcp_port(port) == 0) {
            return port;
        }
    }
    return 0;
}

#if defined(_WIN32)

std::vector<NetworkAddress> enumerate_network_addresses() {
    std::vector<NetworkAddress> result;

    WinsockGuard guard;
    if (!guard.ok()) {
        return result;
    }

    ULONG buffer_size = 16u * 1024u;
    std::vector<uint8_t> buffer(buffer_size);

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;

    ULONG status = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                          reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                                          &buffer_size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        status = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                                        &buffer_size);
    }
    if (status != NO_ERROR) {
        return result;
    }

    for (IP_ADAPTER_ADDRESSES* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {

        // 只保留已启用的网卡；未插网线的适配器地址没有意义。
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }

        // 适配器描述是宽字符，转成 UTF-8 供日志和界面显示。
        std::string interface_name;
        {
            const int wide_length = static_cast<int>(::wcslen(adapter->FriendlyName));
            if (wide_length > 0) {
                const int utf8_length = ::WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName,
                                                              wide_length, nullptr, 0, nullptr, nullptr);
                if (utf8_length > 0) {
                    interface_name.resize(static_cast<std::size_t>(utf8_length));
                    ::WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, wide_length,
                                          interface_name.data(), utf8_length, nullptr, nullptr);
                }
            }
        }

        const bool virtual_adapter = is_virtual_interface(interface_name) ||
                                     adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
                                     adapter->IfType == IF_TYPE_TUNNEL;

        for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
             unicast != nullptr; unicast = unicast->Next) {

            if (unicast->Address.lpSockaddr == nullptr) {
                continue;
            }

            NetworkAddress entry;
            entry.interface_name = interface_name;
            entry.is_virtual = virtual_adapter;

            char text[INET6_ADDRSTRLEN] = {0};

            if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
                const sockaddr_in* ipv4 = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                if (::inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text)) == nullptr) {
                    continue;
                }
                entry.address = text;
                entry.is_ipv6 = false;
                entry.is_loopback = (ipv4->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
            } else if (unicast->Address.lpSockaddr->sa_family == AF_INET6) {
                const sockaddr_in6* ipv6 = reinterpret_cast<const sockaddr_in6*>(unicast->Address.lpSockaddr);
                if (::inet_ntop(AF_INET6, &ipv6->sin6_addr, text, sizeof(text)) == nullptr) {
                    continue;
                }
                entry.address = text;
                entry.is_ipv6 = true;
                entry.is_loopback = IN6_IS_ADDR_LOOPBACK(&ipv6->sin6_addr) != 0;
            } else {
                continue;
            }

            result.push_back(entry);
        }
    }

    return result;
}

std::string primary_local_ipv4() {
    for (const NetworkAddress& entry : broadcastable_addresses()) {
        if (!entry.is_ipv6) {
            return entry.address;
        }
    }
    return std::string();
}

#else  // POSIX

std::vector<NetworkAddress> enumerate_network_addresses() {
    std::vector<NetworkAddress> result;

    struct ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) {
        return result;
    }

    for (struct ifaddrs* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_name == nullptr) {
            continue;
        }

        const int family = entry->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) {
            continue;
        }

        NetworkAddress address;
        address.interface_name = entry->ifa_name;
        address.is_virtual = is_virtual_interface(address.interface_name);

        char text[INET6_ADDRSTRLEN] = {0};

        if (family == AF_INET) {
            const sockaddr_in* ipv4 = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
            if (::inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text)) == nullptr) {
                continue;
            }
            address.address = text;
            address.is_ipv6 = false;
            address.is_loopback = (ipv4->sin_addr.s_addr == htonl(INADDR_LOOPBACK));

            if (entry->ifa_netmask != nullptr) {
                const sockaddr_in* mask = reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask);
                char mask_text[INET_ADDRSTRLEN] = {0};
                if (::inet_ntop(AF_INET, &mask->sin_addr, mask_text, sizeof(mask_text)) != nullptr) {
                    address.netmask = mask_text;
                }
            }
        } else {
            const sockaddr_in6* ipv6 = reinterpret_cast<const sockaddr_in6*>(entry->ifa_addr);
            if (::inet_ntop(AF_INET6, &ipv6->sin6_addr, text, sizeof(text)) == nullptr) {
                continue;
            }
            address.address = text;
            address.is_ipv6 = true;
            address.is_loopback = IN6_IS_ADDR_LOOPBACK(&ipv6->sin6_addr) != 0;

            // 链路本地地址（fe80::）在待机页上显示没意义，用户抄不走。
            if (IN6_IS_ADDR_LINKLOCAL(&ipv6->sin6_addr)) {
                address.is_virtual = true;
            }
        }

        result.push_back(address);
    }

    ::freeifaddrs(interfaces);
    return result;
}

std::string primary_local_ipv4() {
    for (const NetworkAddress& entry : broadcastable_addresses()) {
        if (!entry.is_ipv6) {
            return entry.address;
        }
    }
    return std::string();
}

#endif

std::vector<NetworkAddress> broadcastable_addresses() {
    std::vector<NetworkAddress> result;
    for (const NetworkAddress& entry : enumerate_network_addresses()) {
        if (entry.is_loopback || entry.is_virtual) {
            continue;
        }
        result.push_back(entry);
    }
    return result;
}

}  // namespace adisplay::common
