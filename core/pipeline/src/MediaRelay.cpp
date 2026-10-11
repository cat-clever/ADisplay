#include <adisplay/pipeline/MediaRelay.h>

#include <adisplay/common/Log.h>
#include <adisplay/pipeline/HlsPlaylist.h>
#include <adisplay/pipeline/MediaSource.h>

#include "HttpFetch.h"
#include "RelaySession.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>

namespace adisplay::pipeline {
namespace {

// 拉播放列表的超时。与分片一样，宁可早点失败并记日志，也不让播放器的请求挂着。
constexpr int kPlaylistTimeoutSeconds = 10;

// 播放器会并行要好几份分片，每份都可能阻塞在「等它被生产出来」上；线程不够时
// 后面的请求会排在队列里，表现出来就是播放器卡住不动。
constexpr int kServerThreadCount = 16;

// 启动自检的重试次数与间隔。监听是异步起的，第一次请求可能还连不上。
constexpr int kProbeAttempts = 10;
constexpr int kProbeSleepMs = 50;

// 只接手 http 的播放列表。https 我们拉不了（这份 cpp-httplib 没编进 TLS），
// 而且 Apple 生态里 https 的 HLS 基本都是标准 fMP4，本来也用不着插手。
bool is_relayable(const std::string& url) {
    return looks_like_hls(url) && url.compare(0, 7, "http://") == 0;
}

void send_text(httplib::Response& response, int status, const std::string& message) {
    response.status = status;
    response.set_content(message, "text/plain; charset=utf-8");
}

void send_binary(httplib::Response& response, const std::vector<uint8_t>& data) {
    if (data.empty()) {
        send_text(response, 502, "分片内容为空");
        return;
    }
    response.set_content(reinterpret_cast<const char*>(data.data()), data.size(), "video/mp4");
}

}  // namespace

struct MediaRelay::Impl {
    // 每次 start 都新建一个 Server：httplib 的 Server 停掉之后不能再 listen，
    // 而且路由是累加注册的 —— 复用同一个对象会让每轮启动都多挂一份处理器。
    std::unique_ptr<httplib::Server> server;
    std::thread server_thread;
    std::atomic<bool> running{false};

    mutable std::mutex mutex;
    uint16_t listen_port = 0;
    std::string last_error;
    // 当前会话。旧的不在这里主动停：在跑的请求还握着它的 shared_ptr，
    // 等它们走完自然释放。
    std::shared_ptr<RelaySession> session;

    bool start(std::string* out_error);
    void stop();
    std::string resolve_for_playback(const std::string& remote_url) const;

    void register_routes();
    bool verify_listening() const;

    void handle_local_playlist(const httplib::Request& request, httplib::Response& response);
    void handle_init(httplib::Response& response);
    void handle_segment(const httplib::Request& request, httplib::Response& response);

    std::shared_ptr<RelaySession> current_session() const;

    void set_error(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        last_error = message;
    }

    // 换封装这条路走不通（或远端拿不到）时的统一答复：502 + 日志 + 记下原因。
    void fail(httplib::Response& response, const std::string& message) {
        AD_LOG_ERROR("本地中转：{}", message);
        set_error(message);
        send_text(response, 502, message);
    }
};

bool MediaRelay::Impl::start(std::string* out_error) {
    if (running.load()) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        last_error.clear();
    }

    server = std::make_unique<httplib::Server>();
    server->new_task_queue = [] { return new httplib::ThreadPool(kServerThreadCount); };
    register_routes();

    // 只监听回环：这是给本机播放器用的中转，不该出现在局域网上。
    // 端口交给内核分配再取实际值 —— 写死端口会和别的实例、别的服务撞车。
    const int bound = server->bind_to_any_port("127.0.0.1");
    if (bound <= 0) {
        const std::string message = "本地中转服务绑定端口失败（127.0.0.1）";
        set_error(message);
        if (out_error != nullptr) {
            *out_error = message;
        }
        AD_LOG_ERROR("{}", message);
        server.reset();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        listen_port = static_cast<uint16_t>(bound);
    }

    running.store(true);
    server_thread = std::thread([this]() { server->listen_after_bind(); });

    // 启动自检：本机的 /local.m3u8 至少要能应答「缺 src → 400」。
    // 不做这一步的话，给播放器的地址可能是个死地址，排查时只能看到转圈。
    if (!verify_listening()) {
        const std::string message = "本地中转服务启动自检失败：端口 " +
                                    std::to_string(listen_port) + " 上没有响应";
        set_error(message);
        if (out_error != nullptr) {
            *out_error = message;
        }
        AD_LOG_ERROR("{}", message);
        stop();
        return false;
    }

    AD_LOG_INFO("本地中转服务已就绪：http://127.0.0.1:{}/local.m3u8", listen_port);
    return true;
}

void MediaRelay::Impl::stop() {
    running.store(false);

    // 先让在等的分片请求醒过来：那些线程正卡在等生产，不停会话的话
    // server->stop() 要陪着它们一起等下去。
    std::shared_ptr<RelaySession> active;
    {
        std::lock_guard<std::mutex> lock(mutex);
        active = std::move(this->session);
    }
    if (active != nullptr) {
        active->stop();
    }

    if (server != nullptr) {
        server->stop();
    }
    if (server_thread.joinable()) {
        server_thread.join();
    }
    server.reset();
    {
        std::lock_guard<std::mutex> lock(mutex);
        listen_port = 0;
    }

    AD_LOG_INFO("本地中转服务已停止");
}

std::string MediaRelay::Impl::resolve_for_playback(const std::string& remote_url) const {
    if (remote_url.empty() || !running.load()) {
        return remote_url;
    }
    if (!is_relayable(remote_url)) {
        return remote_url;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (listen_port == 0) {
        return remote_url;
    }
    return "http://127.0.0.1:" + std::to_string(listen_port) + "/local.m3u8?src=" +
           encode_url_parameter(remote_url);
}

std::shared_ptr<RelaySession> MediaRelay::Impl::current_session() const {
    std::lock_guard<std::mutex> lock(mutex);
    return session;
}

void MediaRelay::Impl::register_routes() {
    // 播放列表：这一步才去拉远端，判断要不要接手。
    server->Get("/local.m3u8", [this](const httplib::Request& request, httplib::Response& response) {
        handle_local_playlist(request, response);
    });

    // fMP4 的初始化段（ftyp+moov）。名字与本地播放列表里的 EXT-X-MAP 必须一致。
    server->Get("/" + local_init_name(),
                [this](const httplib::Request&, httplib::Response& response) {
                    handle_init(response);
                });

    server->Get(R"(/seg-(\d+)\.m4s)",
                [this](const httplib::Request& request, httplib::Response& response) {
                    handle_segment(request, response);
                });
}

bool MediaRelay::Impl::verify_listening() const {
    uint16_t probe_port = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        probe_port = listen_port;
    }

    for (int attempt = 0; attempt < kProbeAttempts; ++attempt) {
        httplib::Client probe("127.0.0.1", probe_port);
        probe.set_connection_timeout(0, 300 * 1000);   // 300ms
        probe.set_read_timeout(1, 0);

        // 缺 src 参数时我们回 400。能拿到这个应答，就说明服务在监听、
        // 路由也挂上了 —— 这正是播放器接下来要走的入口。
        const httplib::Result response = probe.Get("/local.m3u8");
        if (response && response->status == 400) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kProbeSleepMs));
    }
    return false;
}

void MediaRelay::Impl::handle_local_playlist(const httplib::Request& request,
                                             httplib::Response& response) {
    const std::string remote = request.get_param_value("src");
    if (remote.empty()) {
        send_text(response, 400, "缺少 src 参数");
        return;
    }

    std::string playlist_url = remote;
    FetchResult playlist = fetch_url(playlist_url, kPlaylistTimeoutSeconds);
    if (!playlist.ok) {
        fail(response, "拉远端播放列表失败：" + playlist.error);
        return;
    }

    SegmentFormat format = classify_playlist(playlist.body);
    if (format == SegmentFormat::MasterPlaylist) {
        // 主列表自己不是分片清单，拉它指向的第一条子列表再判一次。
        const std::string variant = first_variant_url(playlist.body, playlist_url);
        if (variant.empty()) {
            response.set_redirect(remote);
            return;
        }
        FetchResult variant_body = fetch_url(variant, kPlaylistTimeoutSeconds);
        if (!variant_body.ok) {
            fail(response, "拉子播放列表失败：" + variant_body.error);
            return;
        }
        if (variant_body.truncated) {
            AD_LOG_WARN("本地中转：子播放列表不完整（{} 字节），按已收到的部分处理",
                        variant_body.body.size());
        }
        playlist_url = variant;
        playlist.body = std::move(variant_body.body);
        format = classify_playlist(playlist.body);
    }

    if (!needs_local_remux(format)) {
        // 判为 fMP4 或者认不出来：302 回远端，等于没经过我们（零开销）。
        response.set_redirect(remote);
        return;
    }

    const LocalVodPlaylist local = build_local_vod_playlist(playlist.body, playlist_url);
    if (local.body.empty()) {
        // 分片是字节区间、内容加密，或者列表结构不认识 —— 换封装做不了。
        AD_LOG_WARN("本地中转：这份播放列表改不了，退回远端地址");
        response.set_redirect(remote);
        return;
    }

    if (playlist.body.find("#EXT-X-ENDLIST") == std::string::npos) {
        // 直播源：本地列表是按这一轮请求生成的定长清单，源往前走之后它会停在
        // 原地。投屏场景里手机给的代理都是 VOD，遇到直播先记一条日志。
        AD_LOG_WARN("本地中转：源的播放列表没有 EXT-X-ENDLIST，按 VOD 处理");
    }

    std::shared_ptr<RelaySession> active;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (this->session != nullptr && !this->session->stopped() &&
            this->session->playlist_url() == playlist_url) {
            // 同一份源重复拉播放列表（播放器 seek、重载时会这样）就直接复用，
            // 已经产好的分片不必重来。
            active = this->session;
        } else {
            active = std::make_shared<RelaySession>(playlist_url, local.segment_urls,
                                                    local.segment_start_ms);
            this->session = active;
        }
    }

    AD_LOG_INFO("本地中转就绪：{} 份分片（源 {}）", active->segment_count(), playlist_url);
    response.set_content(local.body, "application/vnd.apple.mpegurl");
}

void MediaRelay::Impl::handle_init(httplib::Response& response) {
    const std::shared_ptr<RelaySession> active = current_session();
    if (active == nullptr) {
        send_text(response, 404, "没有正在中转的会话");
        return;
    }

    std::vector<uint8_t> data;
    std::string error;
    if (!active->init_segment(&data, &error)) {
        fail(response, error);
        return;
    }
    send_binary(response, data);
}

void MediaRelay::Impl::handle_segment(const httplib::Request& request,
                                      httplib::Response& response) {
    const std::shared_ptr<RelaySession> active = current_session();
    if (active == nullptr) {
        send_text(response, 404, "没有正在中转的会话");
        return;
    }

    // 路由的正则已经保证这一段全是数字。
    const unsigned long parsed = std::strtoul(request.matches[1].str().c_str(), nullptr, 10);
    const std::size_t index = static_cast<std::size_t>(parsed);

    std::vector<uint8_t> data;
    std::string error;
    if (!active->segment(index, &data, &error)) {
        fail(response, error);
        return;
    }
    send_binary(response, data);
}

// ===========================================================================

MediaRelay::MediaRelay() : impl_(new Impl()) {}

MediaRelay::~MediaRelay() {
    stop();
}

bool MediaRelay::start(std::string* out_error) {
    return impl_->start(out_error);
}

void MediaRelay::stop() {
    impl_->stop();
}

bool MediaRelay::is_running() const {
    return impl_->running.load();
}

std::string MediaRelay::resolve_for_playback(const std::string& remote_url) const {
    return impl_->resolve_for_playback(remote_url);
}

uint16_t MediaRelay::port() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->listen_port;
}

bool MediaRelay::is_relaying() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->running.load() && impl_->session != nullptr && !impl_->session->stopped();
}

std::string MediaRelay::last_error() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->last_error;
}

}  // namespace adisplay::pipeline
