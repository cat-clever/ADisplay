// ADisplay —— DLNA HTTP 端点的集成测试
//
// 守的是一个真实缺陷：cpp-httplib 对内置方法表（GET/HEAD/POST/PUT/DELETE/
// OPTIONS/PATCH）之外的方法，在「解析请求行」阶段就直接回 400，请求到不了
// 任何处理器 —— 连访问日志都不会打一行。
//
// GENA 的 SUBSCRIBE / UNSUBSCRIBE 恰好就是这种方法。后果是 Cling 系控制点
// （天际视频用的就是它）能列出设备、点「投屏」却显示「投屏失败」，并且全程
// 不发任何 POST /control：它必须先订阅成功、拿到 200 + SID + TIMEOUT，才会
// 在「连接成功」回调里去发 SetAVTransportURI。
//
// 所以这条用例必须真的起一次 HTTP 服务、真的发一次 SUBSCRIBE。
// 只测 XML 生成是测不到这个问题的。

#include <adisplay/dlna/DlnaRenderer.h>

#include <httplib.h>

#include <string>

#include "AdTest.h"

namespace {

// 起服务前先问内核要一个空闲端口，避免和别的用例或宿主环境撞车。
// bind_to_any_port 会真的绑定并监听（端口填 0 由内核挑），拿到的端口是真空闲的；
// 用完 stop() 会把监听套接字一并关掉，不会占着不放。
// 这里绑 0.0.0.0，跟 DlnaRenderer 正式启动时保持一致 —— 绑定地址不同的话
// macOS 上同一个端口会被判成占用。
uint16_t pick_free_port() {
    httplib::Server probe;
    const int port = probe.bind_to_any_port("0.0.0.0");
    probe.stop();
    return static_cast<uint16_t>(port < 0 ? 0 : port);
}

// 用 httplib 的客户端发任意方法。客户端写请求行时不校验方法名
// （校验只在服务端那条路径上），所以正好能复现手机发来的 SUBSCRIBE。
httplib::Result send_method(httplib::Client& client, const std::string& method,
                            const std::string& path, const httplib::Headers& headers) {
    httplib::Request request;
    request.method = method;
    request.path = path;
    request.headers = headers;
    return client.send(request);
}

}  // namespace

AD_TEST(dlna_subscribe_is_accepted, "GENA 订阅会被受理并返回 SID 与 TIMEOUT") {
    const uint16_t port = pick_free_port();
    AD_CHECK(port != 0);

    adisplay::dlna::DlnaConfig config;
    config.device_name = "测试设备";
    config.http_port = port;
    config.local_address = "127.0.0.1";

    adisplay::dlna::DlnaRenderer renderer;
    std::string error;
    AD_CHECK(renderer.start(config, &error));
    AD_CHECK_EQ(error, std::string());

    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(3, 0);
    client.set_read_timeout(3, 0);

    // 首次订阅。
    const auto response = send_method(client, "SUBSCRIBE", "/event",
                                      httplib::Headers{
                                          {"CALLBACK", "<http://127.0.0.1:9/notify>"},
                                          {"NT", "upnp:event"},
                                          {"TIMEOUT", "Second-1800"},
                                      });
    AD_CHECK(static_cast<bool>(response));
    // 400 就说明请求在解析阶段被丢了 —— 正是这个缺陷的症状。
    AD_CHECK_EQ(response->status, 200);
    // Cling 判定订阅是否成立，看的就是这两个头；少一个就当作订阅失败。
    AD_CHECK(!response->get_header_value("SID").empty());
    AD_CHECK(!response->get_header_value("TIMEOUT").empty());
    const std::string sid = response->get_header_value("SID");

    // 续订：手机每隔一段时间会带着 SID 重新订一次。
    const auto renew = send_method(client, "SUBSCRIBE", "/event",
                                   httplib::Headers{{"SID", sid}});
    AD_CHECK(static_cast<bool>(renew));
    AD_CHECK_EQ(renew->status, 200);

    // 退订同样不能回 400，否则手机会一直以为订阅还在。
    const auto unsubscribe = send_method(client, "UNSUBSCRIBE", "/event",
                                         httplib::Headers{{"SID", sid}});
    AD_CHECK(static_cast<bool>(unsubscribe));
    AD_CHECK_EQ(unsubscribe->status, 200);

    // 顺带守一下这次改动没把已经能用的端点带坏。
    const auto description = client.Get("/description.xml");
    AD_CHECK(static_cast<bool>(description));
    AD_CHECK_EQ(description->status, 200);

    const auto scpd = client.Get("/scpd/urn:upnp-org:serviceId:AVTransport.xml");
    AD_CHECK(static_cast<bool>(scpd));
    AD_CHECK_EQ(scpd->status, 200);

    renderer.stop();
}

int main() {
    return adtest::run_all("DLNA HTTP 端点测试");
}
