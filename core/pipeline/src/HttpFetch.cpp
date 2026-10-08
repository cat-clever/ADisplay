#include "HttpFetch.h"

#include <httplib.h>

#include <cctype>
#include <cstdlib>
#include <string>

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

    const httplib::Result response = client.Get(target.path.c_str());
    if (!response) {
        result.error = "请求失败：" + std::string(httplib::to_string(response.error()));
        return result;
    }

    result.status = response->status;
    if (response->status != 200) {
        result.error = "远端返回 HTTP " + std::to_string(response->status);
        return result;
    }

    result.ok = true;
    result.body = response->body;
    return result;
}

}  // namespace adisplay::pipeline
