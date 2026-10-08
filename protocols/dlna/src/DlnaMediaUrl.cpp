#include "DlnaMediaUrl.h"

#include <algorithm>
#include <cctype>

namespace adisplay::dlna::media_url {
namespace {

bool is_loopback_host(const std::string& host) {
    std::string lowered = host;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lowered == "localhost") {
        return true;
    }
    if (lowered == "[::1]") {
        return true;
    }
    // 整个 127.0.0.0/8 都是回环，不只 127.0.0.1。
    return lowered.rfind("127.", 0) == 0;
}

// peer_address 可能是 IPv6 字面量（"fe80::1"），拼进 URL 要加方括号。
std::string as_host_literal(const std::string& peer_address) {
    if (peer_address.find(':') != std::string::npos && !peer_address.empty() &&
        peer_address.front() != '[') {
        return "[" + peer_address + "]";
    }
    return peer_address;
}

}  // namespace

std::string rewrite_loopback(const std::string& url, const std::string& peer_address) {
    if (url.empty() || peer_address.empty()) {
        return url;
    }

    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return url;   // 没有协议头，不是我们能安全处理的形态
    }
    const std::size_t host_begin = scheme_end + 3;

    // authority 到第一个 '/'、'?'、'#' 为止。
    std::size_t host_end = url.find_first_of("/?#", host_begin);
    if (host_end == std::string::npos) {
        host_end = url.size();
    }

    const std::string authority = url.substr(host_begin, host_end - host_begin);
    const std::size_t at = authority.rfind('@');
    const std::string userinfo =
        (at == std::string::npos) ? std::string() : authority.substr(0, at + 1);
    const std::string host_port = (at == std::string::npos) ? authority : authority.substr(at + 1);

    // 分离主机名与端口。IPv6 字面量形如 [::1]:7000。
    std::string host;
    std::string port;
    if (!host_port.empty() && host_port.front() == '[') {
        const std::size_t close = host_port.find(']');
        if (close == std::string::npos) {
            return url;
        }
        host = host_port.substr(0, close + 1);
        port = host_port.substr(close + 1);
    } else {
        const std::size_t colon = host_port.find(':');
        if (colon == std::string::npos) {
            host = host_port;
        } else {
            host = host_port.substr(0, colon);
            port = host_port.substr(colon);
        }
    }

    if (!is_loopback_host(host)) {
        return url;
    }

    return url.substr(0, host_begin) + userinfo + as_host_literal(peer_address) + port +
           url.substr(host_end);
}

}  // namespace adisplay::dlna::media_url
