// ADisplay —— 远端拉取（HTTP 客户端）
//
// 为什么不用 httplib 的客户端（它就在这里，用它更省事）：
//
// 手机上的投屏代理是「边拉上游边转发」的实现，正文会分很多次、中间带停顿地
// 送过来。httplib 在读取超时或提前 EOF 时一律报 Error::Read，并且**把已经收到
// 的那部分正文丢掉** —— 于是我们手里只剩一句「读失败」，既不知道收了多少字节、
// 也不知道是对端提前关了还是我们等得不耐烦了。中转这条路径前前后后改了三轮
// 请求头都没找对方向，就是因为这个：没有可观测性时，人只能在错误的地方试。
//
// 这个实现要做的是同一件事，但把过程留下来：
//   * 读超时按「多久没有新字节」算，慢速但活着的传输不会被误杀
//   * 正文短于 Content-Length 时，用 Range 续传补齐；补不齐就把已收到的留下
//   * 无论成败，字节数、耗时、是否超时都写进日志
//
// 这里不做 TLS。中转的目标是手机在局域网里开的明文 HTTP 代理，
// 以及它给出的明文分片地址。

#include "HttpFetch.h"

#include <adisplay/common/Log.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <errno.h>
#  include <netdb.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

namespace adisplay::pipeline {
namespace {

// 单次抓取的绝对上限。
//
// 读超时（下面按「多久没有新字节」算）保证不会因为对端卡住而挂死；这个上限防的
// 是另一种情况 —— 对端一直慢悠悠地送字节，我们就这样被占着一条线程。中转是在
// 播放器的请求线程上同步跑的，不能无限期陪着。
constexpr int kOverallBudgetSeconds = 120;

// 正文短于声明长度时，最多续传几次。一次续传拿回一小段也算进展，
// 所以给得宽一些；但要有上限，否则对端每次都只回几个字节就成了死循环。
constexpr int kMaxContinuations = 16;

constexpr int kMaxRedirects = 3;

constexpr std::size_t kReadBufferSize = 16 * 1024;

struct HttpTarget {
    bool valid = false;
    std::string host;
    int port = 80;
    std::string path;
    std::string error;
};

// 拆出主机、端口与路径。这一段自己写而不交给第三方：不同实现对「带路径的完整
// URL」支持不一致，而我们要的行为很确定，也必须能对 IPv6 字面量和 user:pass@
// 这两种写法给出明确结果。
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
        // 这份构建没有编 TLS，https 拉不了。调用方（MediaRelay）本来也只把
        // http 的播放列表转到本地，走到这里说明是意外情况。
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

    // IPv6 字面量写作 [::1]:7000，冒号的切分点在方括号之后。
    if (authority[0] == '[') {
        const std::size_t bracket = authority.find(']');
        if (bracket == std::string::npos) {
            target.error = "IPv6 地址缺少右方括号：" + url;
            return target;
        }
        target.host = authority.substr(1, bracket - 1);
        const std::size_t colon = authority.find(':', bracket);
        if (colon != std::string::npos) {
            target.port = std::atoi(authority.c_str() + colon + 1);
        }
    } else {
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
        }
    }

    if (target.host.empty()) {
        target.error = "地址里没有主机名：" + url;
        return target;
    }
    if (target.port <= 0 || target.port > 65535) {
        target.error = "地址里的端口不合法：" + url;
        return target;
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

int64_t now_ms() {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// 头与正文的分界。\r\n\r\n 是规范写法，但极简实现里 \n\n 也出现过 ——
// 手机上的代理恰恰属于「极简实现」这一类。
std::size_t find_header_end(const std::string& data) {
    const std::size_t crlf = data.find("\r\n\r\n");
    if (crlf != std::string::npos) {
        return crlf + 4;
    }
    const std::size_t lf = data.find("\n\n");
    if (lf != std::string::npos) {
        return lf + 2;
    }
    return std::string::npos;
}

int parse_status_code(const std::string& headers) {
    const std::size_t space = headers.find(' ');
    if (space == std::string::npos) {
        return 0;
    }
    return std::atoi(headers.c_str() + space + 1);
}

// 按名字取一个头字段的值，大小写不敏感。取不到返回空串。
std::string header_value(const std::string& headers, const std::string& name) {
    const std::string wanted = lower_ascii(name);
    std::size_t pos = headers.find('\n');
    if (pos == std::string::npos) {
        return std::string();
    }
    ++pos;

    while (pos < headers.size()) {
        std::size_t line_end = headers.find('\n', pos);
        if (line_end == std::string::npos) {
            line_end = headers.size();
        }
        std::string line = headers.substr(pos, line_end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return std::string();
        }
        if (lower_ascii(line.substr(0, colon)) == wanted) {
            std::string value = line.substr(colon + 1);
            std::size_t begin = 0;
            while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) {
                ++begin;
            }
            return value.substr(begin);
        }
        pos = line_end + 1;
    }
    return std::string();
}

bool parse_hex_ll(const std::string& text, long long* out) {
    if (text.empty()) {
        return false;
    }
    long long value = 0;
    for (char c : text) {
        int digit = -1;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = 10 + (c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            digit = 10 + (c - 'A');
        } else {
            return false;
        }
        value = value * 16 + digit;
    }
    *out = value;
    return true;
}

// 解 chunked 正文。
//
// 收全了返回 true 且 *complete 为真；数据还没到齐返回 true 且 *complete 为假；
// 格式不对返回 false。
bool decode_chunked(const std::string& raw, std::string* out, bool* complete) {
    out->clear();
    *complete = false;

    std::size_t pos = 0;
    while (true) {
        const std::size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos) {
            return true;   // 长度行还没到齐
        }
        std::string size_text = raw.substr(pos, eol - pos);
        // 长度后面可以跟 ";扩展"，忽略它。
        const std::size_t semi = size_text.find(';');
        if (semi != std::string::npos) {
            size_text = size_text.substr(0, semi);
        }
        long long size = 0;
        if (!parse_hex_ll(size_text, &size) || size < 0) {
            return false;
        }
        pos = eol + 2;
        if (size == 0) {
            *complete = true;
            return true;
        }
        if (raw.size() < pos + static_cast<std::size_t>(size)) {
            return true;   // 这块还没收全
        }
        out->append(raw, pos, static_cast<std::size_t>(size));
        pos += static_cast<std::size_t>(size);
        if (pos + 1 >= raw.size()) {
            return true;   // 分隔的 CRLF 还没到
        }
        if (raw[pos] != '\r' || raw[pos + 1] != '\n') {
            return false;
        }
        pos += 2;
    }
}

// 只留可见字符，避免把二进制塞进日志。
std::string readable_prefix(const std::string& data, std::size_t limit) {
    std::string readable;
    for (char ch : data) {
        const unsigned char u = static_cast<unsigned char>(ch);
        readable.push_back((u >= 0x20 && u < 0x7f) ? ch : '.');
        if (readable.size() >= limit) {
            break;
        }
    }
    return readable;
}

#if defined(_WIN32)
using RawSocket = SOCKET;
const RawSocket kInvalidSocket = INVALID_SOCKET;

void close_raw_socket(RawSocket fd) {
    ::closesocket(fd);
}

void ensure_sockets_ready() {
    // WSAStartup 可以重复调用，引用计数由 winsock 自己维护。
    WSADATA wsa_data;
    (void)::WSAStartup(MAKEWORD(2, 2), &wsa_data);
}

bool last_error_is_interrupted() {
    return false;   // Windows 的 recv 不会因信号返回 EINTR
}

bool last_error_is_timeout() {
    const int code = ::WSAGetLastError();
    return code == WSAETIMEDOUT || code == WSAEWOULDBLOCK;
}

bool last_error_is_would_block() {
    return ::WSAGetLastError() == WSAEWOULDBLOCK;
}
#else
using RawSocket = int;
const RawSocket kInvalidSocket = -1;

void close_raw_socket(RawSocket fd) {
    ::close(fd);
}

void ensure_sockets_ready() {}

bool last_error_is_interrupted() {
    return errno == EINTR;
}

bool last_error_is_timeout() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

bool last_error_is_would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}
#endif

RawSocket connect_to(const HttpTarget& target, int timeout_seconds, std::string* error) {
    ensure_sockets_ready();

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* addresses = nullptr;
    const std::string port_text = std::to_string(target.port);
    if (::getaddrinfo(target.host.c_str(), port_text.c_str(), &hints, &addresses) != 0 ||
        addresses == nullptr) {
        *error = "解析主机失败：" + target.host;
        return kInvalidSocket;
    }

    RawSocket fd = kInvalidSocket;
    int last_error = 0;
    for (struct addrinfo* it = addresses; it != nullptr; it = it->ai_next) {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == kInvalidSocket) {
            continue;
        }

        // 连接也要有超时。默认的阻塞 connect 在内网里通常一两毫秒就返回，
        // 但对端关机或地址被防火墙丢弃时会挂到系统默认的几十秒 —— 中转是在
        // 播放器的请求线程上跑的，那段时间里整个播放会话都卡着。
#if defined(_WIN32)
        const DWORD connect_timeout_ms = static_cast<DWORD>(timeout_seconds) * 1000;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char*>(&connect_timeout_ms),
                           sizeof(connect_timeout_ms));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                           reinterpret_cast<const char*>(&connect_timeout_ms),
                           sizeof(connect_timeout_ms));
#else
        struct timeval connect_timeout;
        connect_timeout.tv_sec = timeout_seconds;
        connect_timeout.tv_usec = 0;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &connect_timeout,
                           sizeof(connect_timeout));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &connect_timeout,
                           sizeof(connect_timeout));
#endif

        if (::connect(fd, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
            break;
        }

#if defined(_WIN32)
        last_error = ::WSAGetLastError();
#else
        last_error = errno;
#endif
        close_raw_socket(fd);
        fd = kInvalidSocket;
    }
    ::freeaddrinfo(addresses);

    if (fd == kInvalidSocket) {
        *error = "连不上 " + target.host + ":" + std::to_string(target.port) +
                 "（错误码 " + std::to_string(last_error) + "）";
        return kInvalidSocket;
    }
    return fd;
}

void apply_read_timeout(RawSocket fd, int timeout_seconds) {
    // 这个超时是「多久没有新字节」，不是「整个响应必须在多久内读完」——
    // 内核每收到一段数据就重新计时。手机上的代理是边拉上游边转发的，
    // 用它来卡整个响应，只会把一次正常的慢速传输误判成失败。
#if defined(_WIN32)
    const DWORD timeout_ms = static_cast<DWORD>(timeout_seconds) * 1000;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    struct timeval timeout;
    timeout.tv_sec = timeout_seconds;
    timeout.tv_usec = 0;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

// 往一个已被对端关掉的 socket 上写会触发 SIGPIPE，在 macOS/Linux 上那是直接
// 杀进程 —— 一个拉流失败不该让整个程序消失。
void apply_send_guards(RawSocket fd) {
#if defined(SO_NOSIGPIPE)
    int no_sigpipe = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#else
    (void) fd;
#endif
}

int send_flags() {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

bool send_all(RawSocket fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int chunk = static_cast<int>(
            data.size() - sent > 64 * 1024 ? 64 * 1024 : data.size() - sent);
        const int written = static_cast<int>(
            ::send(fd, data.data() + sent, chunk, send_flags()));
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && (last_error_is_interrupted() || last_error_is_would_block())) {
            continue;
        }
        return false;
    }
    return true;
}

struct RawResponse {
    int status = 0;
    std::string headers;
    std::string body;
    // -1 表示响应里没有 Content-Length（chunked 或读到连接关闭为止）。
    long long declared_length = -1;
    bool chunked = false;
    // 正文是否完整。不完整不等于失败 —— 已收到的部分仍然可能有用。
    bool complete = false;
    bool timed_out = false;
    int64_t elapsed_ms = 0;
    std::string error;
};

// 发一次请求并读完响应。不做重定向、不做续传 —— 那些是 fetch_url 的事。
RawResponse perform_request(const HttpTarget& target, const std::string& extra_headers,
                            int timeout_seconds) {
    RawResponse response;
    const int64_t started = now_ms();

    std::string connect_error;
    RawSocket fd = connect_to(target, timeout_seconds, &connect_error);
    if (fd == kInvalidSocket) {
        response.error = connect_error;
        response.elapsed_ms = now_ms() - started;
        return response;
    }
    apply_read_timeout(fd, timeout_seconds);
    apply_send_guards(fd);

    // 请求头逐项都是有用的，不是照抄：
    //   Host         —— HTTP/1.1 必填，缺了多数服务端直接 400
    //   Connection: close —— 让对端在正文结束后关连接，我们不必猜什么时候读完
    //   Accept-Encoding: identity —— 不谈压缩。有的代理对 gzip 处理不当会掐断连接，
    //                        而我们要的播放列表与分片本来就是压缩不了的
    //   User-Agent   —— 一部分代理会按 UA 判断是不是播放器
    std::string request = "GET " + target.path + " HTTP/1.1\r\n";
    request += "Host: " + target.host;
    if (target.port != 80) {
        request += ":" + std::to_string(target.port);
    }
    request += "\r\n";
    request += "User-Agent: ADisplay\r\n";
    request += "Accept: */*\r\n";
    request += "Accept-Encoding: identity\r\n";
    request += "Connection: close\r\n";
    request += extra_headers;
    request += "\r\n";

    if (!send_all(fd, request)) {
        close_raw_socket(fd);
        response.error = "发送请求失败";
        response.elapsed_ms = now_ms() - started;
        return response;
    }

    std::string received;
    bool eof = false;
    bool timed_out = false;
    std::size_t header_end = std::string::npos;
    long long content_length = -1;
    bool chunked = false;
    bool complete = false;
    bool over_budget = false;

    const int64_t deadline = started + static_cast<int64_t>(kOverallBudgetSeconds) * 1000;
    char buffer[kReadBufferSize];

    while (true) {
        if (now_ms() > deadline) {
            over_budget = true;
            break;
        }

        const int got = static_cast<int>(::recv(fd, buffer, sizeof(buffer), 0));
        if (got > 0) {
            received.append(buffer, static_cast<std::size_t>(got));
        } else if (got == 0) {
            eof = true;
            break;
        } else if (last_error_is_interrupted()) {
            continue;
        } else {
            timed_out = last_error_is_timeout();
            break;
        }

        if (header_end == std::string::npos) {
            header_end = find_header_end(received);
            if (header_end == std::string::npos) {
                continue;   // 头还没收全
            }
            const std::string headers = received.substr(0, header_end);
            const std::string length_text = header_value(headers, "Content-Length");
            if (!length_text.empty()) {
                content_length = std::atoll(length_text.c_str());
            }
            const std::string encoding = lower_ascii(header_value(headers, "Transfer-Encoding"));
            chunked = encoding.find("chunked") != std::string::npos;
        }

        const std::size_t body_size = received.size() - header_end;
        if (chunked) {
            // 分块正文的结束标志是最后那块长度为 0。检查尾部而不是整段解码，
            // 是为了不在每次 recv 之后都做一遍 O(n) 的解析。
            if ((received.size() >= 5 &&
                 received.compare(received.size() - 5, 5, "0\r\n\r\n") == 0) ||
                (received.size() >= 7 &&
                 received.compare(received.size() - 7, 7, "\r\n0\r\n\r\n") == 0)) {
                complete = true;
                break;
            }
        } else if (content_length >= 0) {
            if (static_cast<long long>(body_size) >= content_length) {
                complete = true;
                break;
            }
        }
        // 既没有 Content-Length 也不是分块：只能读到对端关连接为止。
    }

    close_raw_socket(fd);
    response.elapsed_ms = now_ms() - started;
    response.timed_out = timed_out;

    if (received.empty()) {
        response.error = timed_out ? "等待响应超时（一个字节都没收到）"
                                   : "对端在响应前关闭了连接";
        return response;
    }
    if (header_end == std::string::npos) {
        // 连 HTTP 头都不完整，说明对面回的根本不是 HTTP。把开头记下来 ——
        // 这一条曾经把「代理回的是一段错误页」和「协议对不上」区分开过。
        response.error = "响应不是完整的 HTTP 报文，收到 " +
                         std::to_string(received.size()) + " 字节：" +
                         readable_prefix(received, 200);
        return response;
    }

    response.headers = received.substr(0, header_end);
    response.status = parse_status_code(response.headers);
    response.declared_length = content_length;
    response.chunked = chunked;

    const std::string raw_body = received.substr(header_end);
    if (chunked) {
        std::string decoded;
        bool decoded_complete = false;
        if (!decode_chunked(raw_body, &decoded, &decoded_complete)) {
            response.error = "分块正文格式不对（收到 " + std::to_string(raw_body.size()) + " 字节）";
            return response;
        }
        response.body = std::move(decoded);
        response.complete = decoded_complete;
    } else {
        response.body = raw_body;
        if (content_length >= 0) {
            if (static_cast<long long>(response.body.size()) > content_length) {
                // 对端多发了。有的是把下一条响应粘上来了，有的就是实现有问题。
                // 多出来的字节不是我们要的正文，截掉。
                response.body.resize(static_cast<std::size_t>(content_length));
            }
            response.complete =
                static_cast<long long>(response.body.size()) >= content_length;
        } else {
            response.complete = eof;
        }
    }

    if (!response.complete && over_budget) {
        response.error = "超过单次抓取的时间上限（" +
                         std::to_string(kOverallBudgetSeconds) + " 秒）";
    }
    return response;
}

bool is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// 相对地址补成绝对地址（规范里 Location 允许是相对路径）。
std::string resolve_location(const std::string& base_url, const std::string& location) {
    if (location.compare(0, 7, "http://") == 0 || location.compare(0, 8, "https://") == 0) {
        return location;
    }
    const std::size_t scheme_end = base_url.find("://");
    if (scheme_end == std::string::npos) {
        return location;
    }
    const std::size_t authority_end = base_url.find('/', scheme_end + 3);
    const std::string origin =
        authority_end == std::string::npos ? base_url : base_url.substr(0, authority_end);
    if (!location.empty() && location[0] == '/') {
        return origin + location;
    }
    // 相对当前目录：退到最后一个 '/' 之前。
    const std::size_t last_slash = base_url.rfind('/');
    if (last_slash == std::string::npos || last_slash < scheme_end + 3) {
        return origin + "/" + location;
    }
    return base_url.substr(0, last_slash + 1) + location;
}

}  // namespace

// 失败时再看一眼对端到底回了什么。
//
// 「读失败」这类报错的信息量为零：连接建立了，但对端发了什么、发了多少，
// 从外面完全看不出来。这里用最朴素的 HTTP/1.0 请求（不谈压缩、不带多余头、
// 连完即关）再探一次，把开头若干字节记进日志。
//
// 有一点必须说清楚，否则会误导判断：它**最多读 4 次、每次 512 字节**就停，
// 所以「收到 N 字节」只说明对端至少发了 N 字节，**不能**据此推断对端提前关了
// 连接 —— 我自己就据此误判过一次。要判断是提前关闭还是慢速传输，看
// perform_request 留下的「声明长度 / 实际收到 / 是否超时」那一组数字。
std::string probe_raw_response(const std::string& host, int port, const std::string& path) {
    HttpTarget target;
    target.valid = true;
    target.host = host;
    target.port = port;
    target.path = path;

    std::string error;
    RawSocket fd = connect_to(target, 5, &error);
    if (fd == kInvalidSocket) {
        return "探针：" + error;
    }
    apply_read_timeout(fd, 5);
    apply_send_guards(fd);

    const std::string request = "GET " + path + " HTTP/1.0\r\nHost: " + host +
                                "\r\nConnection: close\r\n\r\n";
    if (!send_all(fd, request)) {
        close_raw_socket(fd);
        return "探针：请求发不出去";
    }

    std::string received;
    char buffer[512];
    for (int i = 0; i < 4; ++i) {
        const int got = static_cast<int>(::recv(fd, buffer, sizeof(buffer), 0));
        if (got <= 0) {
            break;
        }
        received.append(buffer, static_cast<std::size_t>(got));
    }
    close_raw_socket(fd);

    if (received.empty()) {
        return "探针：连上了但对端一个字节都没回";
    }
    return "探针收到 " + std::to_string(received.size()) + " 字节（最多只读 4 次）" +
           readable_prefix(received, 200);
}

FetchResult fetch_url(const std::string& url, int timeout_seconds) {
    FetchResult result;

    std::string current_url = url;
    for (int redirect = 0; redirect <= kMaxRedirects; ++redirect) {
        const HttpTarget target = parse_http_url(current_url);
        if (!target.valid) {
            result.error = target.error;
            return result;
        }

        RawResponse response = perform_request(target, std::string(), timeout_seconds);
        if (response.status == 0) {
            result.error = "请求失败：" +
                           (response.error.empty() ? std::string("没有响应") : response.error) +
                           "；" + probe_raw_response(target.host, target.port, target.path);
            return result;
        }

        if (is_redirect(response.status)) {
            const std::string location = header_value(response.headers, "Location");
            if (location.empty() || redirect == kMaxRedirects) {
                result.status = response.status;
                result.error = "远端返回 HTTP " + std::to_string(response.status) +
                               "（跳转）但没给出可用的 Location";
                return result;
            }
            current_url = resolve_location(current_url, location);
            continue;
        }

        if (response.status != 200 && response.status != 206) {
            // 把内容类型一并带上：跳到别处、还是回了个错误页，从这里能看出来。
            result.status = response.status;
            result.error = "远端返回 HTTP " + std::to_string(response.status) + "（" +
                           header_value(response.headers, "Content-Type") + "）";
            return result;
        }

        result.status = response.status;
        std::string body = std::move(response.body);
        const long long expected = response.declared_length;
        bool complete = response.complete;
        int continuations = 0;

        // 正文短于声明长度：用 Range 从断点续传。
        //
        // 手机上的代理是「边拉上游边转发」的，一次只送一段就关连接并不罕见。
        // 它既然在响应里声明了完整长度，就说明它知道总共有多少 —— 那续传在
        // 协议上是说得通的。不支持的话，下面的判断会让循环停下来。
        while (!complete && !body.empty() && expected > 0 &&
               static_cast<long long>(body.size()) < expected &&
               continuations < kMaxContinuations) {
            ++continuations;
            const std::string range_header =
                "Range: bytes=" + std::to_string(body.size()) + "-\r\n";

            RawResponse more = perform_request(target, range_header, timeout_seconds);
            if (more.status == 206) {
                if (more.body.empty()) {
                    break;   // 有进展才继续，否则就是死循环
                }
                body += more.body;
                complete = static_cast<long long>(body.size()) >= expected;
            } else if (more.status == 200) {
                // 对端不支持 Range，又把整份发了一遍。不能再往上接（会重复），
                // 但它这次可能发完了 —— 用它这一份更完整的话就换掉。
                if (more.body.size() > body.size()) {
                    body = std::move(more.body);
                    complete = static_cast<long long>(body.size()) >= expected;
                }
                break;
            } else {
                // 416（范围越界）或别的错误：就用已经拿到的。
                break;
            }
        }

        if (!complete) {
            // 拿到的可能只是开头，但仍然是有用的 —— 一份被截断的播放列表
            // 至少能让播放器起播前面那些分片，比「完全没画面」强。
            //
            // 这一行是这条路径上最重要的日志：它把「对端提前关了」和「我们等得
            // 不耐烦了」区分开（看 timed_out），也给出了差多少字节。
            AD_LOG_WARN("本地中转：远端响应不完整，声明 {} 字节、实收 {} 字节，"
                        "续传 {} 次后放弃{}；耗时 {} ms",
                        expected, body.size(), continuations,
                        response.timed_out ? "（读取超时）" : "（对端提前关闭或超过总时长上限）",
                        response.elapsed_ms);
            result.truncated = true;
        } else {
            AD_LOG_DEBUG("本地中转：远端拉取成功，{} 字节，耗时 {} ms",
                         body.size(), response.elapsed_ms);
        }

        result.ok = true;
        result.body = std::move(body);
        return result;
    }

    result.error = "跳转次数过多";
    return result;
}

}  // namespace adisplay::pipeline
