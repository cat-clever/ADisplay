#include "HttpFetch.h"

#include <httplib.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <netdb.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace adisplay::pipeline {
namespace {

struct HttpTarget {
    bool valid = false;
    std::string host;
    int port = 80;
    std::string path;
    std::string error;
};

// 拆出主机、端口与路径。这一段自己写而不交给 httplib：不同版本的构造函数
// 对「带路径的完整 URL」支持不一致，而我们要的行为很确定。
HttpTarget parse_http_url(const std::string& url) {
    HttpTarget target;

    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        target.error = "地址缺少协议头：" + url;
        return target;
    }

    std::string scheme = url.substr(0, scheme_end);
    for (char& c : scheme) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (scheme != "http") {
        // 这份 httplib 没有编进 TLS，https 拉不了。调用方（MediaRelay）本来
        // 也只把 http 的播放列表转到本地，走到这里说明是意外情况。
        target.error = "不支持的协议（这份构建没有 TLS）：" + scheme;
        return target;
    }

    const std::size_t host_begin = scheme_end + 3;
    std::size_t host_end = url.find_first_of("/?#", host_begin);
    if (host_end == std::string::npos) {
        host_end = url.size();
    }
    std::string authority = url.substr(host_begin, host_end - host_begin);

    // 去掉 user:pass@，我们不支持带认证的地址。
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        authority = authority.substr(at + 1);
    }
    if (authority.empty()) {
        target.error = "地址里没有主机名：" + url;
        return target;
    }

    const std::size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
        target.host = authority;
    } else {
        target.host = authority.substr(0, colon);
        const std::string port_text = authority.substr(colon + 1);
        if (port_text.empty()) {
            target.error = "地址里的端口是空的：" + url;
            return target;
        }
        target.port = std::atoi(port_text.c_str());
        if (target.port <= 0 || target.port > 65535) {
            target.error = "地址里的端口不合法：" + port_text;
            return target;
        }
    }

    if (host_end >= url.size()) {
        target.path = "/";
    } else {
        const std::size_t path_begin = url.find_first_of("/?", host_end);
        target.path = path_begin == std::string::npos ? "/" : url.substr(path_begin);
    }

    target.valid = true;
    return target;
}

}  // namespace


#if defined(_WIN32)
using RawSocket = SOCKET;
inline void close_raw_socket(RawSocket fd) {
    ::closesocket(fd);
}
#else
using RawSocket = int;
inline void close_raw_socket(RawSocket fd) {
    ::close(fd);
}
#endif

// 失败时的原始探针。
//
// 「读失败」这类报错的信息量为零：连接建立了，但对端到底回了什么、什么也没回，
// 从外面完全看不出来。这里用最朴素的 HTTP/1.0 请求（不谈压缩、不带多余头、
// 连完即关）再探一次，把对端实际发回来的头若干字节记进日志。
//
// 目的不是靠它取到数据，而是把猜测换成事实 —— 与给 Windows 加启动日志同一个思路：
// 没有可观测性时，人只能在错误的方向上来回试。
std::string probe_raw_response(const std::string& host, int port, const std::string& path) {
#if defined(_WIN32)
    WSADATA wsa_data;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return "探针：WSAStartup 失败";
    }
#endif

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* addresses = nullptr;
    const std::string port_text = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addresses) != 0 ||
        addresses == nullptr) {
        return "探针：解析主机失败";
    }

    RawSocket fd = static_cast<RawSocket>(-1);
    for (struct addrinfo* it = addresses; it != nullptr; it = it->ai_next) {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == static_cast<RawSocket>(-1)) {
            continue;
        }
        if (::connect(fd, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
            break;
        }
        close_raw_socket(fd);
        fd = static_cast<RawSocket>(-1);
    }
    ::freeaddrinfo(addresses);

    if (fd == static_cast<RawSocket>(-1)) {
        return "探针：连不上";
    }

    const std::string request = "GET " + path + " HTTP/1.0\r\nHost: " + host +
                                "\r\nConnection: close\r\n\r\n";
    // 往一个已被对端关掉的 socket 上写会触发 SIGPIPE，在 macOS/Linux 上那是直接
    // 杀进程。探针是为了排查问题，绝不能把自己搞崩 —— 两个平台各有各的关法。
#if defined(SO_NOSIGPIPE)
    int no_sigpipe = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
#if defined(MSG_NOSIGNAL)
    const int send_flags = MSG_NOSIGNAL;
#else
    const int send_flags = 0;
#endif
    (void)::send(fd, request.data(), static_cast<int>(request.size()), send_flags);

    std::string received;
    char buffer[512];
    for (int i = 0; i < 4; ++i) {
        const int got = static_cast<int>(::recv(fd, buffer, sizeof(buffer), 0));
        if (got <= 0) {
            break;
        }
        received.append(buffer, static_cast<std::size_t>(got));
        if (received.size() >= sizeof(buffer) * 4) {
            break;
        }
    }
    close_raw_socket(fd);

    if (received.empty()) {
        return "探针：连上了但对端一个字节都没回";
    }

    // 只留可见字符，避免把二进制塞进日志。
    std::string readable;
    for (char ch : received) {
        const unsigned char u = static_cast<unsigned char>(ch);
        readable.push_back((u >= 0x20 && u < 0x7f) ? ch : '.');
        if (readable.size() >= 200) {
            break;
        }
    }
    return "探针收到 " + std::to_string(received.size()) + " 字节：" + readable;
}

FetchResult fetch_url(const std::string& url, int timeout_seconds) {
    FetchResult result;

    const HttpTarget target = parse_http_url(url);
    if (!target.valid) {
        result.error = target.error;
        return result;
    }

    httplib::Client client(target.host, target.port);
    client.set_connection_timeout(timeout_seconds, 0);
    client.set_read_timeout(timeout_seconds, 0);
    // 远端代理偶尔会跳一次（例如带签名的地址先 302 到真实分片）。
    client.set_follow_location(true);

    // 请求头里有两处是刻意压掉的，都是为了迁就手机上的本地代理：
    //
    //   Connection: close —— 那类代理多是极简实现，应答不带 Content-Length 又
    //                       不关连接时，客户端会一直等，最后报成「读失败」。
    //   Accept-Encoding: identity —— 不去谈压缩。这份 httplib 编了 brotli，
    //                       默认会带 Accept-Encoding: br，而有些代理对 br/gzip
    //                       处理不当会直接掐断连接。
    //
    // 判据来自一次对照：同一台手机同一个地址，macOS 上的 AVPlayer 是能取到数据的
    // （当时表现为有声音没画面），所以代理确实对外服务 —— 差别只可能在请求头。
    const httplib::Headers headers = {
        {"Connection", "close"},
        {"Accept-Encoding", "identity"},
        {"Accept", "*/*"},
    };

    const httplib::Result response = client.Get(target.path.c_str(), headers);
    if (!response) {
        result.error = "请求失败：" + std::string(httplib::to_string(response.error())) + "；" +
                       probe_raw_response(target.host, target.port, target.path);
        return result;
    }

    result.status = response->status;
    if (response->status != 200) {
        // 把内容类型一并带上：302 到别处、还是回了个错误页，从这里能看出来。
        result.error = "远端返回 HTTP " + std::to_string(response->status) + "（" +
                       response->get_header_value("Content-Type") + "）";
        return result;
    }

    result.ok = true;
    result.body = response->body;
    return result;
}

}  // namespace adisplay::pipeline
