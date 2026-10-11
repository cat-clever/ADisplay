// ADisplay —— DLNA 媒体地址规范化的测试
//
// 守的是「手机推来回环地址」这条真实路径：天际视频会在自己手机上跑一个
// 带签名的本地代理，然后把 http://127.0.0.1:7000/resource.m3u8?... 推给
// 接收端。对接收端来说 127.0.0.1 是它自己，不改写就永远拉不到，
// 而且报出来的是一句「You do not have permission to access ...」，
// 完全看不出根因。
//
// 这段逻辑只有几十行，但改写时必须原样保留端口、路径、查询串 —— 查询串里
// 装着签名，动一个字符服务器就拒。所以逐项钉住。

#include <adisplay/dlna/DlnaRenderer.h>

#include <string>

#include "AdTest.h"
#include "DlnaMediaUrl.h"

namespace {

std::string rewrite(const std::string& url, const std::string& peer) {
    return adisplay::dlna::media_url::rewrite_loopback(url, peer);
}

}  // namespace

AD_TEST(media_url_loopback_rewritten, "回环地址改用发送端，端口与查询串原样保留") {
    const std::string url =
        "http://127.0.0.1:7000/resource.m3u8?src=aHR0cA==&ck=abc&Time=12345";
    AD_CHECK_EQ(rewrite(url, "172.20.10.2"),
                std::string("http://172.20.10.2:7000/resource.m3u8?src=aHR0cA==&ck=abc&Time=12345"));
}

AD_TEST(media_url_localhost_and_other_127, "localhost 与 127 段的其它地址同样改写") {
    AD_CHECK_EQ(rewrite("http://localhost:8080/a.m3u8", "10.0.0.5"),
                std::string("http://10.0.0.5:8080/a.m3u8"));
    AD_CHECK_EQ(rewrite("http://127.0.0.53/x", "10.0.0.5"),
                std::string("http://10.0.0.5/x"));
    AD_CHECK_EQ(rewrite("https://127.0.0.1:443/s", "10.0.0.5"),
                std::string("https://10.0.0.5:443/s"));
}

AD_TEST(media_url_public_host_untouched, "正常地址一律不动") {
    const std::string url = "http://cn-zjjh-ct-04-03.bilivideo.com/x.mp4?e=1";
    AD_CHECK_EQ(rewrite(url, "172.20.10.2"), url);
    // 127 出现在路径里不代表主机名是回环。
    AD_CHECK_EQ(rewrite("http://10.0.0.9/127.0.0.1.mp4", "172.20.10.2"),
                std::string("http://10.0.0.9/127.0.0.1.mp4"));
}

AD_TEST(media_url_without_peer_untouched, "拿不到发送端地址时不猜") {
    AD_CHECK_EQ(rewrite("http://127.0.0.1:7000/a.m3u8", ""),
                std::string("http://127.0.0.1:7000/a.m3u8"));
}

AD_TEST(media_url_ipv6_peer_bracketed, "IPv6 发送端要加方括号") {
    AD_CHECK_EQ(rewrite("http://127.0.0.1:7000/a.m3u8", "fe80::1"),
                std::string("http://[fe80::1]:7000/a.m3u8"));
    AD_CHECK_EQ(rewrite("http://[::1]:7000/a.m3u8", "fe80::1"),
                std::string("http://[fe80::1]:7000/a.m3u8"));
}

int main() {
    return adtest::run_all("DLNA 媒体地址测试");
}
