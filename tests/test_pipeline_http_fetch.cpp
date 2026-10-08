// ADisplay —— 远端拉取的测试
//
// 守的是手机端投屏代理那套「边拉上游边转发」的行为。它在读取超时或提前关闭
// 连接时的表现，直接决定了中转能不能把播放列表拿全 —— 而这一条路径前前后后
// 改了三轮请求头都没找对方向，因为旧实现（httplib 客户端）在出错时会把已经
// 收到的正文丢掉，只剩一句「读失败」。
//
// 所以这里用一个能精确控制响应的裸 socket 假服务端，把这几种情形各自钉住：
//   * 正文短于声明的长度、对端随即关闭（最常见的一种）
//   * 对端支持 Range 时能不能续传补齐
//   * 分块传输
//   * 跳转
//   * HTTP/1.0 风格：没有 Content-Length，读到关闭为止
//
// 断言里既看正文，也看 truncated 标记 —— 后者决定调用方是「凑合用」还是
// 「这一片不要了」，判断错了比拿不到数据更难排查。

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include "AdTest.h"
#include "HttpFetch.h"

namespace {

using adisplay::pipeline::FetchResult;
using adisplay::pipeline::fetch_url;

#if defined(_WIN32)
using RawSocket = SOCKET;
const RawSocket kInvalidSocket = INVALID_SOCKET;
void close_socket(RawSocket fd) { ::closesocket(fd); }
void sockets_init() {
    WSADATA data;
    (void)::WSAStartup(MAKEWORD(2, 2), &data);
}
#else
using RawSocket = int;
const RawSocket kInvalidSocket = -1;
void close_socket(RawSocket fd) { ::close(fd); }
void sockets_init() {}
#endif

bool send_all(RawSocket fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#if defined(MSG_NOSIGNAL)
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        const int written = static_cast<int>(
            ::send(fd, data.data() + sent, static_cast<int>(data.size() - sent), flags));
        if (written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

// 一个可以精确控制原始响应的假服务端。
//
// 用它而不是 httplib 的服务端，是因为要测的恰恰是 httplib 表达不出来的东西：
// 「声明 100 字节但只发 20 字节然后关连接」。
class RawStubServer {
public:
    using Handler = std::function<std::string(const std::string& request)>;

    explicit RawStubServer(Handler handler) : handler_(std::move(handler)) {
        sockets_init();

        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ == kInvalidSocket) {
            return;
        }
        int reuse = 1;
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                           reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        struct sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        address.sin_port = 0;   // 让系统挑一个空闲端口
        if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = kInvalidSocket;
            return;
        }
        if (::listen(listen_fd_, 8) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = kInvalidSocket;
            return;
        }

        struct sockaddr_in bound;
#if defined(_WIN32)
        int bound_len = sizeof(bound);
#else
        socklen_t bound_len = sizeof(bound);
#endif
        std::memset(&bound, 0, sizeof(bound));
        if (::getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&bound), &bound_len) == 0) {
            port_ = ::ntohs(bound.sin_port);
        }

        thread_ = std::thread([this]() { serve(); });
    }

    ~RawStubServer() {
        stopped_.store(true);
        if (listen_fd_ != kInvalidSocket) {
            // accept 会立刻返回错误，服务线程随即退出。
            close_socket(listen_fd_);
            listen_fd_ = kInvalidSocket;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    RawStubServer(const RawStubServer&) = delete;
    RawStubServer& operator=(const RawStubServer&) = delete;

    bool valid() const { return port_ != 0; }
    uint16_t port() const { return port_; }
    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

private:
    void serve() {
        while (!stopped_.load()) {
            struct sockaddr_in peer;
#if defined(_WIN32)
            int peer_len = sizeof(peer);
#else
            socklen_t peer_len = sizeof(peer);
#endif
            const RawSocket client =
                ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&peer), &peer_len);
            if (client == kInvalidSocket) {
                return;   // 监听 socket 已关，收摊
            }

            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos) {
                const int got = static_cast<int>(::recv(client, buffer, sizeof(buffer), 0));
                if (got <= 0) {
                    break;
                }
                request.append(buffer, static_cast<std::size_t>(got));
            }

            const std::string response = handler_(request);
            (void)send_all(client, response);
            close_socket(client);
        }
    }

    Handler handler_;
    RawSocket listen_fd_ = kInvalidSocket;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stopped_{false};
};

std::string http_header(int status, const std::string& extra) {
    std::string text = "HTTP/1.1 " + std::to_string(status) + " OK\r\n";
    text += extra;
    text += "Connection: close\r\n\r\n";
    return text;
}

std::string make_body(std::size_t size, char fill) {
    return std::string(size, fill);
}

bool request_has_range(const std::string& request) {
    return request.find("Range: bytes=") != std::string::npos;
}

std::size_t range_offset(const std::string& request) {
    const std::size_t at = request.find("Range: bytes=");
    if (at == std::string::npos) {
        return 0;
    }
    return static_cast<std::size_t>(std::atoll(request.c_str() + at + 13));
}

}  // namespace

AD_TEST(http_fetch_complete_body, "声明多少就收多少：正文完整，不算截断") {
    const std::string body = make_body(4096, 'A');
    RawStubServer server([&body](const std::string&) {
        return http_header(200, "Content-Length: " + std::to_string(body.size()) + "\r\n") + body;
    });
    AD_CHECK(server.valid());

    const FetchResult result = fetch_url(server.url("/resource.m3u8"), 5);
    AD_CHECK(result.ok);
    AD_CHECK_EQ(result.status, 200);
    AD_CHECK_EQ(result.body.size(), body.size());
    AD_CHECK(!result.truncated);
}

AD_TEST(http_fetch_truncated_body, "对端提前关闭且不支持续传：留下已收到的部分并标记截断") {
    const std::string wanted = make_body(4096, 'B');
    const std::string sent = wanted.substr(0, 1000);

    RawStubServer server([&wanted, &sent](const std::string& request) {
        // 第一次如实声明完整长度却只发一小段；续传请求回整份前缀，
        // 模拟「不支持 Range 的对端」。
        if (request_has_range(request)) {
            return http_header(200, "Content-Length: " + std::to_string(wanted.size()) + "\r\n") + sent;
        }
        return http_header(200, "Content-Length: " + std::to_string(wanted.size()) + "\r\n") + sent;
    });
    AD_CHECK(server.valid());

    const FetchResult result = fetch_url(server.url("/resource.m3u8"), 5);

    // 拿不到全部也要成功返回。调用方（MediaRelay）据此起播前面那些分片 ——
    // 比「完全没有画面」强，前提是把截断这件事如实标出来。
    AD_CHECK(result.ok);
    AD_CHECK_EQ(result.body, sent);
    AD_CHECK(result.truncated);
}

AD_TEST(http_fetch_range_continuation, "对端支持 Range：续传补齐，最终不算截断") {
    std::string body;
    for (int i = 0; i < 4096; ++i) {
        body.push_back(static_cast<char>('a' + (i % 26)));
    }

    RawStubServer server([&body](const std::string& request) {
        if (!request_has_range(request)) {
            // 只送前 1000 字节就关连接。
            return http_header(200, "Content-Length: " + std::to_string(body.size()) + "\r\n") +
                   body.substr(0, 1000);
        }
        const std::size_t offset = range_offset(request);
        const std::string rest = body.substr(offset);
        return http_header(206,
                           "Content-Length: " + std::to_string(rest.size()) + "\r\n"
                           "Content-Range: bytes " + std::to_string(offset) + "-" +
                               std::to_string(body.size() - 1) + "/" +
                               std::to_string(body.size()) + "\r\n") +
               rest;
    });
    AD_CHECK(server.valid());

    const FetchResult result = fetch_url(server.url("/resource.m3u8"), 5);

    AD_CHECK(result.ok);
    AD_CHECK_EQ(result.body, body);
    AD_CHECK(!result.truncated);
}

AD_TEST(http_fetch_chunked_body, "分块传输：解出正文，不算截断") {
    RawStubServer server([](const std::string&) {
        std::string response = http_header(200, "Transfer-Encoding: chunked\r\n");
        response += "5\r\n#EXTM\r\n";
        response += "4\r\n3U\r\n\r\n";   // 故意让分块边界落在正文中间
        response += "0\r\n\r\n";
        return response;
    });
    AD_CHECK(server.valid());

    const FetchResult result = fetch_url(server.url("/resource.m3u8"), 5);

    AD_CHECK(result.ok);
    AD_CHECK_EQ(result.body, std::string("#EXTM3U\r\n"));
    AD_CHECK(!result.truncated);
}

AD_TEST(http_fetch_without_length, "没有 Content-Length：读到对端关闭为止，视为完整") {
    const std::string body = "#EXTM3U\n#EXT-X-VERSION:6\n";
    RawStubServer server([&body](const std::string&) {
        // HTTP/1.0 风格：不声明长度，发完就关。
        return std::string("HTTP/1.0 200 OK\r\nConnection: close\r\n\r\n") + body;
    });
    AD_CHECK(server.valid());

    const FetchResult result = fetch_url(server.url("/resource.m3u8"), 5);

    AD_CHECK(result.ok);
    AD_CHECK_EQ(result.body, body);
    // 没有声明长度就无从判断「短了」，按读完即完整处理。
    AD_CHECK(!result.truncated);
}

AD_TEST(http_fetch_follows_redirect, "跳转：跟随 Location 再取一次") {
    const std::string body = "#EXTM3U\n#EXT-X-ENDLIST\n";
    RawStubServer server([&body](const std::string& request) {
        if (request.find("/real.m3u8") != std::string::npos) {
            return http_header(200, "Content-Length: " + std::to_string(body.size()) + "\r\n") + body;
        }
        // 相对地址的 Location —— 规范允许，实际上也常见。
        return http_header(302, "Location: /real.m3u8\r\n");
    });
    AD_CHECK(server.valid());

    const FetchResult result = fetch_url(server.url("/resource.m3u8"), 5);

    AD_CHECK(result.ok);
    AD_CHECK_EQ(result.status, 200);
    AD_CHECK_EQ(result.body, body);
}

AD_TEST(http_fetch_reports_status, "非 2xx：如实报出状态码与内容类型") {
    RawStubServer server([](const std::string&) {
        return http_header(404, "Content-Type: text/plain\r\nContent-Length: 3\r\n") + "no\n";
    });
    AD_CHECK(server.valid());

    const FetchResult result = fetch_url(server.url("/missing.m3u8"), 5);

    AD_CHECK(!result.ok);
    AD_CHECK_EQ(result.status, 404);
    // 错误信息里要带内容类型：跳到别处、还是回了个错误页，靠它区分。
    AD_CHECK(result.error.find("404") != std::string::npos);
    AD_CHECK(result.error.find("text/plain") != std::string::npos);
}

AD_TEST(http_fetch_rejects_non_http, "对端回的不是 HTTP：错误里带上收到的开头") {
    RawStubServer server([](const std::string&) {
        return std::string("HTTP/1.1 200 OK\r\n");   // 头没结束就断了
    });
    AD_CHECK(server.valid());

    const FetchResult result = fetch_url(server.url("/resource.m3u8"), 5);

    AD_CHECK(!result.ok);
    // 这一条把「代理回的是一段错误页」和「协议对不上」区分开过，
    // 所以错误信息里必须留下对端实际发的字节。
    AD_CHECK(result.error.find("HTTP/1.1 200 OK") != std::string::npos);
}

AD_TEST(http_fetch_rejects_bad_url, "地址不合法：不发起请求，直接给出原因") {
    const FetchResult https_result = fetch_url("https://example.com/a.m3u8", 5);
    AD_CHECK(!https_result.ok);
    AD_CHECK(https_result.error.find("TLS") != std::string::npos);

    const FetchResult relative_result = fetch_url("/a.m3u8", 5);
    AD_CHECK(!relative_result.ok);
    AD_CHECK(!relative_result.error.empty());
}
