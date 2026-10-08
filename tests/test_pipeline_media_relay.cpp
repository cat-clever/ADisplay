// ADisplay —— 本地中转服务的端到端测试
//
// 守的是接进 DLNA 之前最容易出错的三件事：
//   1. 「给播放器的地址」这个函数必须立刻返回、不发任何网络请求 ——
//      它是在 SetAVTransportURI 的应答里被调用的，慢一点手机就判超时断开
//   2. 播放列表请求要真的读一遍远端那份再判断：fMP4 或者认不出来就 302 回远端
//      （零开销），只有 TS 才生成本地 VOD 列表
//   3. 生成本地列表之后，会话要进入「正在中转」状态，名字与路由对得上
//
// 换封装那一段（需要真实 TS 素材）不在这里测：CI 上没有素材。所以用例都停在
// 「本地列表已生成」为止，不会去请求 /init.mp4 与分片。

#include <adisplay/pipeline/HlsPlaylist.h>
#include <adisplay/pipeline/MediaRelay.h>

#include <httplib.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "AdTest.h"

namespace {

using adisplay::pipeline::encode_url_parameter;
using adisplay::pipeline::MediaRelay;

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// 一个只回固定正文的小服务，顶替手机那个带签名的本地代理。
class StubUpstream {
public:
    StubUpstream(const std::string& body) {
        server_.Get("/resource.m3u8",
                    [body](const httplib::Request&, httplib::Response& response) {
                        response.set_content(body, "application/vnd.apple.mpegurl");
                    });
        const int bound = server_.bind_to_any_port("127.0.0.1");
        port_ = static_cast<uint16_t>(bound < 0 ? 0 : bound);
        thread_ = std::thread([this]() { server_.listen_after_bind(); });

        // 监听是异步起的，先探到能应答再交给用例，否则第一个请求会撞上连接失败。
        for (int attempt = 0; attempt < 50; ++attempt) {
            httplib::Client probe("127.0.0.1", port_);
            probe.set_connection_timeout(0, 200 * 1000);
            if (probe.Get("/resource.m3u8")) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    ~StubUpstream() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    StubUpstream(const StubUpstream&) = delete;
    StubUpstream& operator=(const StubUpstream&) = delete;

    // 带查询串：手机给的地址就是这种带签名的形态。
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/resource.m3u8?m8=signature&sz=1";
    }

private:
    httplib::Server server_;
    std::thread thread_;
    uint16_t port_ = 0;
};

// 问内核要一个当前没人监听的端口，用来构造「远端连不上」。
uint16_t pick_free_port() {
    httplib::Server probe;
    const int port = probe.bind_to_any_port("127.0.0.1");
    probe.stop();
    return static_cast<uint16_t>(port < 0 ? 0 : port);
}

const char* kTsPlaylist =
    "#EXTM3U\n"
    "#EXT-X-VERSION:6\n"
    "#EXT-X-TARGETDURATION:8\n"
    "#EXT-X-PLAYLIST-TYPE:VOD\n"
    "#EXTINF:7.680000,\n"
    "output_000000.ts?m8=63b8ac135a965f1d&sz=295348\n"
    "#EXTINF:5.000000,\n"
    "output_000001.ts?m8=1008c285880afbb7&sz=211312\n";

const char* kFmp4Playlist =
    "#EXTM3U\n"
    "#EXT-X-VERSION:7\n"
    "#EXT-X-MAP:URI=\"init.mp4\"\n"
    "#EXTINF:6.000000,\n"
    "seg-1.m4s\n";

}  // namespace

AD_TEST(relay_resolve_without_start, "没启动时地址原样返回，也不去连任何东西") {
    MediaRelay relay;
    AD_CHECK(!relay.is_running());
    AD_CHECK_EQ(relay.port(), static_cast<uint16_t>(0));

    const std::string hls = "http://172.20.10.2:7000/resource.m3u8?m8=1";
    AD_CHECK_EQ(relay.resolve_for_playback(hls), hls);
    AD_CHECK_EQ(relay.resolve_for_playback("http://x/y.mp4"), std::string("http://x/y.mp4"));
    AD_CHECK_EQ(relay.last_error(), std::string());
    AD_CHECK(!relay.is_relaying());
}

AD_TEST(relay_resolve_rewrites_hls_only, "启动后只有 http 的 HLS 换成本地地址") {
    MediaRelay relay;
    std::string error;
    AD_CHECK(relay.start(&error));
    AD_CHECK_EQ(error, std::string());
    AD_CHECK(relay.is_running());
    AD_CHECK(relay.port() != 0);

    const std::string expected =
        "http://127.0.0.1:" + std::to_string(relay.port()) +
        "/local.m3u8?src=http%3A%2F%2F172.20.10.2%3A7000%2Fresource.m3u8%3Fm8%3D1%26sz%3D2";
    AD_CHECK_EQ(relay.resolve_for_playback("http://172.20.10.2:7000/resource.m3u8?m8=1&sz=2"),
                expected);

    // 直连 mp4 不换；https 的播放列表也不换 —— 这份 httplib 没有 TLS，拉不了，
    // 而 Apple 生态里 https 的 HLS 本来就是标准 fMP4。
    AD_CHECK_EQ(relay.resolve_for_playback("http://x/y.mp4"), std::string("http://x/y.mp4"));
    AD_CHECK_EQ(relay.resolve_for_playback("https://x/y.m3u8"),
                std::string("https://x/y.m3u8"));

    AD_CHECK(!relay.is_relaying());
    relay.stop();
    relay.stop();   // 幂等：DLNA 停两次是常态（会话结束 + 引擎关闭）
    AD_CHECK(!relay.is_running());
    AD_CHECK_EQ(relay.port(), static_cast<uint16_t>(0));

    // 停了之后必须原样返回，否则界面会拿着一个已经关掉的端口去播。
    const std::string hls = "http://172.20.10.2:7000/resource.m3u8";
    AD_CHECK_EQ(relay.resolve_for_playback(hls), hls);
}

AD_TEST(relay_local_playlist_for_ts_source, "TS 源生成指向本地的 VOD 列表") {
    StubUpstream upstream(kTsPlaylist);
    AD_CHECK(!upstream.url().empty());

    MediaRelay relay;
    std::string error;
    AD_CHECK(relay.start(&error));

    httplib::Client client("127.0.0.1", relay.port());
    client.set_connection_timeout(3, 0);
    client.set_read_timeout(10, 0);

    const auto response =
        client.Get(("/local.m3u8?src=" + encode_url_parameter(upstream.url())).c_str());
    AD_CHECK(static_cast<bool>(response));
    AD_CHECK_EQ(response->status, 200);

    // 播放器要能在这里拿到 init 段的名字与分片清单。
    AD_CHECK(contains(response->body, "#EXT-X-MAP:URI=\"init.mp4\"\n"));
    AD_CHECK(contains(response->body, "#EXT-X-PLAYLIST-TYPE:VOD\n"));
    AD_CHECK(contains(response->body, "#EXTINF:7.680,\nseg-0.m4s\n"));
    AD_CHECK(contains(response->body, "#EXTINF:5.000,\nseg-1.m4s\n"));
    AD_CHECK(contains(response->body, "#EXT-X-ENDLIST\n"));
    // 本地列表里不该再出现远端地址 —— 出现就说明播放器会绕过我们的换封装。
    AD_CHECK_EQ(response->body.find("output_000000.ts"), std::string::npos);

    AD_CHECK(relay.is_relaying());

    // 同一份源再拉一次播放列表：直接复用会话，不重建。
    const auto again =
        client.Get(("/local.m3u8?src=" + encode_url_parameter(upstream.url())).c_str());
    AD_CHECK(static_cast<bool>(again));
    AD_CHECK_EQ(again->status, 200);
    AD_CHECK_EQ(again->body, response->body);

    relay.stop();
    AD_CHECK(!relay.is_relaying());
}

AD_TEST(relay_redirects_when_not_ts, "fMP4 与认不出来的源都 302 回远端") {
    MediaRelay relay;
    std::string error;
    AD_CHECK(relay.start(&error));

    httplib::Client client("127.0.0.1", relay.port());
    client.set_connection_timeout(3, 0);
    client.set_read_timeout(10, 0);
    client.set_follow_location(false);   // 要看的就是那一条 302 本身

    {
        StubUpstream upstream(kFmp4Playlist);
        const auto response =
            client.Get(("/local.m3u8?src=" + encode_url_parameter(upstream.url())).c_str());
        AD_CHECK(static_cast<bool>(response));
        AD_CHECK_EQ(response->status, 302);
        AD_CHECK_EQ(response->get_header_value("Location"), upstream.url());
    }
    // 不是播放列表的源（认不出分片封装）同样退回远端。
    {
        StubUpstream upstream("#EXTM3U\n#EXT-X-ENDLIST\n");
        const auto response =
            client.Get(("/local.m3u8?src=" + encode_url_parameter(upstream.url())).c_str());
        AD_CHECK(static_cast<bool>(response));
        AD_CHECK_EQ(response->status, 302);
    }

    // 一次都没接手过，界面上不该显示「正在中转」。
    AD_CHECK(!relay.is_relaying());
    relay.stop();
}

AD_TEST(relay_bad_requests, "缺参数、远端拿不到、没有会话时都明确失败") {
    MediaRelay relay;
    std::string error;
    AD_CHECK(relay.start(&error));

    httplib::Client client("127.0.0.1", relay.port());
    client.set_connection_timeout(3, 0);
    client.set_read_timeout(15, 0);

    // 缺 src：启动自检看的就是这一条。
    const auto missing = client.Get("/local.m3u8");
    AD_CHECK(static_cast<bool>(missing));
    AD_CHECK_EQ(missing->status, 400);

    // 没有会话就请求 init 段 —— 播放器乱序请求时会出现。
    const auto early = client.Get("/init.mp4");
    AD_CHECK(static_cast<bool>(early));
    AD_CHECK_EQ(early->status, 404);

    // 远端连不上：要明确回 502，并把原因记下来（日志与界面都靠它）。
    const uint16_t dead_port = pick_free_port();
    AD_CHECK(dead_port != 0);
    const std::string dead = "http://127.0.0.1:" + std::to_string(dead_port) + "/resource.m3u8";
    const auto unreachable = client.Get(("/local.m3u8?src=" + encode_url_parameter(dead)).c_str());
    AD_CHECK(static_cast<bool>(unreachable));
    AD_CHECK_EQ(unreachable->status, 502);
    AD_CHECK(!relay.last_error().empty());

    relay.stop();
}

int main() {
    return adtest::run_all("本地中转服务测试");
}
