// ADisplay —— DLNA 渲染器实现（HTTP 服务 + SOAP 分发 + GENA 事件）
#include <adisplay/dlna/DlnaRenderer.h>

#include <adisplay/common/DeviceIdentity.h>   // generate_uuid_v4，订阅 SID 要用
#include <adisplay/common/DeviceName.h>
#include <adisplay/common/Log.h>
#include <adisplay/common/NetUtil.h>
#include <adisplay/common/Random.h>

#include "DlnaDescription.h"
#include "DlnaSoap.h"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdlib>   // atoi，解析 SetVolume 的音量
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace adisplay::dlna {
namespace {

namespace soap_ns = adisplay::dlna::soap;
namespace description_ns = adisplay::dlna::description;

// GENA 订阅的默认有效期。手机一般会带 TIMEOUT 头自己指定，没带就用这个。
constexpr int kDefaultSubscriptionSeconds = 1800;

// 事件推送的超时。手机端如果卡住，不能把我们的线程一起拖住。
constexpr int kEventPushTimeoutSeconds = 5;

// 事件序列号从 1 开始，0 表示「首次推送」，规范要求如此。
constexpr uint32_t kInitialEventSequence = 0;

std::string to_string_trimmed(int value) {
    return std::to_string(value);
}

}  // namespace

// ===========================================================================

struct DlnaRenderer::Impl {
    DlnaConfig config;
    IDlnaListener* listener = nullptr;

    httplib::Server server;
    std::thread server_thread;
    std::atomic<bool> running{false};
    std::atomic<uint64_t> actions_handled{0};

    mutable std::mutex mutex;
    std::string last_error;
    std::string base_url;

    // 当前媒体。SetAVTransportURI 记下来，GetMediaInfo 要回给手机。
    std::string current_uri;
    std::string current_metadata;

    // ---- GENA 订阅 --------------------------------------------------------

    struct Subscription {
        std::string sid;
        std::string callback_url;
        std::chrono::steady_clock::time_point expires_at;
        uint32_t sequence = kInitialEventSequence;
    };
    std::vector<Subscription> subscriptions;

    // ---- 事件推送 ---------------------------------------------------------

    // 向所有订阅者推一条属性变更通知。
    //
    // GENA 的事件体形如：
    //   <e:propertyset><e:property><TransportState>PLAYING</TransportState>
    //   </e:property></e:propertyset>
    //
    // 手机端的进度条、音量滑块靠它更新。状态没变时不要推 —— 每秒都推会让
    // 手机端不停重绘，反而卡顿。
    void push_event(const std::string& service_type,
                    const std::vector<std::pair<std::string, std::string>>& properties) {
        std::vector<Subscription> targets;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto now = std::chrono::steady_clock::now();
            for (Subscription& subscription : subscriptions) {
                if (subscription.expires_at <= now) {
                    continue;   // 过期的订阅，顺手跳过
                }
                ++subscription.sequence;
                targets.push_back(subscription);
            }
        }
        if (targets.empty()) {
            return;
        }

        std::ostringstream body;
        body << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
             << "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">\n";
        for (const auto& property : properties) {
            body << "<e:property><" << property.first << ">"
                 << soap_ns::escape_xml(property.second)
                 << "</" << property.first << "></e:property>\n";
        }
        body << "</e:propertyset>\n";

        const std::string payload = body.str();

        for (const Subscription& target : targets) {
            // 回调 URL 形如 "<http://192.168.1.30:12345/notify>"，可能带尖括号。
            std::string url = target.callback_url;
            if (!url.empty() && url.front() == '<') {
                url.erase(0, 1);
            }
            if (!url.empty() && url.back() == '>') {
                url.pop_back();
            }

            const std::size_t scheme_end = url.find("://");
            if (scheme_end == std::string::npos) {
                continue;
            }
            const std::size_t host_begin = scheme_end + 3;
            const std::size_t path_begin = url.find('/', host_begin);
            const std::string host =
                url.substr(host_begin, (path_begin == std::string::npos ? url.size() : path_begin) - host_begin);
            const std::string path =
                path_begin == std::string::npos ? std::string("/") : url.substr(path_begin);

            httplib::Client client(host);
            client.set_connection_timeout(kEventPushTimeoutSeconds, 0);
            client.set_read_timeout(kEventPushTimeoutSeconds, 0);

            httplib::Headers headers = {
                {"NT", "upnp:event"},
                {"NTS", "upnp:propchange"},
                {"SID", target.sid},
                {"SEQ", to_string_trimmed(static_cast<int>(target.sequence))},
                {"CONTENT-TYPE", "text/xml; charset=\"utf-8\""},
            };

            // 推失败不做重试：手机端下一轮 SUBSCRIBE 会重新订阅，
            // 而且它自己也会周期性拉 GetPositionInfo 兜底。
            const auto response = client.Post(path.c_str(), headers, payload, "text/xml; charset=\"utf-8\"");
            if (!response) {
                AD_LOG_DEBUG("事件推送失败（手机可能已断开）：{}", target.callback_url);
            }
        }
    }

    // ---- SOAP 动作分发 ----------------------------------------------------

    // 每个动作返回「输出参数名 → 值」的有序列表。顺序应当与 SCPD 里声明的
    // 一致 —— 部分手机端按位置取值。
    using ActionResult = std::vector<std::pair<std::string, std::string>>;

    bool dispatch_av_transport(const soap_ns::ActionRequest& request,
                               ActionResult* results, int* error_code, std::string* error_text) {
        const std::string& action = request.action_name;

        if (action == "SetAVTransportURI") {
            const std::string uri = request.get("CurrentURI");
            const std::string metadata = request.get("CurrentURIMetaData");
            if (uri.empty()) {
                *error_code = soap_ns::error_code::kInvalidArgs;
                *error_text = "CurrentURI 为空";
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                current_uri = uri;
                current_metadata = metadata;
            }
            AD_LOG_INFO("手机推送媒体：{}", description_ns::extract_title_from_metadata(metadata));
            if (listener != nullptr) {
                listener->on_set_uri(uri, metadata);
            }
            return true;   // 无输出参数
        }

        if (action == "Play") {
            if (listener != nullptr) {
                listener->on_play();
            }
            return true;
        }
        if (action == "Pause") {
            if (listener != nullptr) {
                listener->on_pause();
            }
            return true;
        }
        if (action == "Stop") {
            if (listener != nullptr) {
                listener->on_stop();
            }
            return true;
        }

        if (action == "Seek") {
            // Unit 可能是 REL_TIME / ABS_TIME / TRACK_NR。
            // 只处理时间单位，TRACK_NR 对单曲播放没有意义。
            const std::string unit = request.get("Unit");
            const std::string target = request.get("Target");
            if (unit != "REL_TIME" && unit != "ABS_TIME") {
                *error_code = soap_ns::error_code::kInvalidArgs;
                *error_text = "不支持的 Seek 单位：" + unit;
                return false;
            }
            const int64_t position_ms = soap_ns::parse_duration(target);
            if (position_ms < 0) {
                *error_code = soap_ns::error_code::kInvalidArgs;
                *error_text = "无法解析 Seek 目标：" + target;
                return false;
            }
            if (listener != nullptr) {
                listener->on_seek(position_ms);
            }
            return true;
        }

        if (action == "GetTransportInfo") {
            const std::string state = listener != nullptr
                                          ? listener->current_transport_state()
                                          : std::string(transport_state::kStopped);
            results->emplace_back("CurrentTransportState",
                                  state.empty() ? transport_state::kStopped : state);
            results->emplace_back("CurrentTransportStatus", "OK");
            results->emplace_back("CurrentSpeed", "1");
            return true;
        }

        if (action == "GetMediaInfo") {
            std::string uri;
            std::string metadata;
            {
                std::lock_guard<std::mutex> lock(mutex);
                uri = current_uri;
                metadata = current_metadata;
            }
            const int64_t duration = listener != nullptr ? listener->media_duration_ms() : 0;

            results->emplace_back("NrTracks", uri.empty() ? "0" : "1");
            results->emplace_back("MediaDuration", soap_ns::format_duration(duration));
            results->emplace_back("CurrentURI", uri);
            results->emplace_back("CurrentURIMetaData", metadata);
            results->emplace_back("NextURI", "");
            results->emplace_back("NextURIMetaData", "");
            results->emplace_back("PlayMedium", "NETWORK");
            results->emplace_back("RecordMedium", "NOT_IMPLEMENTED");
            results->emplace_back("WriteStatus", "NOT_IMPLEMENTED");
            return true;
        }

        if (action == "GetPositionInfo") {
            const int64_t position = listener != nullptr ? listener->current_position_ms() : 0;
            const int64_t duration = listener != nullptr ? listener->media_duration_ms() : 0;

            std::string uri;
            std::string metadata;
            {
                std::lock_guard<std::mutex> lock(mutex);
                uri = current_uri;
                metadata = current_metadata;
            }

            results->emplace_back("Track", "1");
            results->emplace_back("TrackDuration", soap_ns::format_duration(duration));
            results->emplace_back("TrackMetaData", metadata);
            results->emplace_back("TrackURI", uri);
            // RelTime 是手机端进度条的主要依据，必须给准。
            results->emplace_back("RelTime", soap_ns::format_duration(position));
            results->emplace_back("AbsTime", soap_ns::format_duration(position));
            results->emplace_back("RelCount", "2147483647");
            results->emplace_back("AbsCount", "2147483647");
            return true;
        }

        if (action == "GetTransportSettings") {
            results->emplace_back("PlayMode", "NORMAL");
            results->emplace_back("RecQualityMode", "NOT_IMPLEMENTED");
            return true;
        }

        if (action == "GetDeviceCapabilities") {
            results->emplace_back("PlayMedia", "NETWORK,NONE");
            results->emplace_back("RecMedia", "NOT_IMPLEMENTED");
            results->emplace_back("RecQualityModes", "NOT_IMPLEMENTED");
            return true;
        }

        *error_code = soap_ns::error_code::kInvalidAction;
        *error_text = "不支持的动作：" + action;
        return false;
    }

    bool dispatch_rendering_control(const soap_ns::ActionRequest& request,
                                    ActionResult* results, int* error_code, std::string* error_text) {
        const std::string& action = request.action_name;

        if (action == "GetVolume") {
            const int volume = listener != nullptr ? listener->current_volume() : 100;
            results->emplace_back("CurrentVolume", to_string_trimmed(volume));
            return true;
        }

        if (action == "SetVolume") {
            const std::string desired = request.get("DesiredVolume");
            if (desired.empty()) {
                *error_code = soap_ns::error_code::kInvalidArgs;
                *error_text = "缺少 DesiredVolume";
                return false;
            }
            const int volume = std::atoi(desired.c_str());
            if (volume < 0 || volume > 100) {
                *error_code = soap_ns::error_code::kArgumentValueOutOfRange;
                *error_text = "音量超出 0..100：" + desired;
                return false;
            }
            if (listener != nullptr) {
                listener->on_set_volume(volume);
            }
            return true;
        }

        if (action == "GetMute") {
            const bool mute = listener != nullptr && listener->current_mute();
            results->emplace_back("CurrentMute", mute ? "1" : "0");
            return true;
        }

        if (action == "SetMute") {
            const std::string desired = request.get("DesiredMute");
            const bool mute = desired == "1" || desired == "true" || desired == "True";
            if (listener != nullptr) {
                listener->on_set_mute(mute);
            }
            return true;
        }

        *error_code = soap_ns::error_code::kInvalidAction;
        *error_text = "不支持的动作：" + action;
        return false;
    }

    bool dispatch_connection_manager(const soap_ns::ActionRequest& request,
                                     ActionResult* results, int* error_code, std::string* error_text) {
        const std::string& action = request.action_name;

        if (action == "GetProtocolInfo") {
            // Source 留空：我们不往外推流。Sink 列出能接收的格式，
            // 手机端据此判断要不要把某个视频投过来。
            results->emplace_back("Source", "");
            results->emplace_back("Sink",
                "http-get:*:video/mp4:*,"
                "http-get:*:video/x-matroska:*,"
                "http-get:*:video/webm:*,"
                "http-get:*:video/mpeg:*,"
                "http-get:*:application/vnd.apple.mpegurl:*,"
                "http-get:*:application/x-mpegurl:*,"
                "http-get:*:audio/mpeg:*,"
                "http-get:*:audio/mp4:*,"
                "http-get:*:audio/flac:*,"
                "http-get:*:image/jpeg:*,"
                "http-get:*:image/png:*");
            return true;
        }

        if (action == "GetCurrentConnectionIDs") {
            results->emplace_back("ConnectionIDs", "0");
            return true;
        }

        if (action == "GetCurrentConnectionInfo") {
            results->emplace_back("RcsID", "0");
            results->emplace_back("AVTransportID", "0");
            results->emplace_back("ProtocolInfo", "");
            results->emplace_back("PeerConnectionManager", "");
            results->emplace_back("PeerConnectionID", "-1");
            results->emplace_back("Direction", "Input");
            results->emplace_back("Status", "OK");
            return true;
        }

        *error_code = soap_ns::error_code::kInvalidAction;
        *error_text = "不支持的动作：" + action;
        return false;
    }

    // ---- HTTP 处理 --------------------------------------------------------

    void handle_description(httplib::Response& response) {
        const std::string xml = description_ns::build_device_description(config, base_url);
        response.set_content(xml, "text/xml; charset=\"utf-8\"");
    }

    void handle_scpd(const std::string& service_id, httplib::Response& response) {
        // SCPDURL 形如 /scpd/urn:upnp-org:serviceId:AVTransport.xml
        std::string service_type;
        if (service_id.find("AVTransport") != std::string::npos) {
            service_type = description_ns::kAvTransportType;
        } else if (service_id.find("RenderingControl") != std::string::npos) {
            service_type = description_ns::kRenderingControlType;
        } else if (service_id.find("ConnectionManager") != std::string::npos) {
            service_type = description_ns::kConnectionManagerType;
        }

        const std::string xml = description_ns::build_service_description(service_type);
        if (xml.empty()) {
            response.status = 404;
            return;
        }
        response.set_content(xml, "text/xml; charset=\"utf-8\"");
    }

    void handle_control(const httplib::Request& request, httplib::Response& response) {
        const std::string soap_action = request.get_header_value("SOAPAction");

        soap_ns::ActionRequest parsed;
        std::string parse_error;
        if (!soap_ns::parse_action_request(soap_action, request.body, &parsed, &parse_error)) {
            AD_LOG_WARN("SOAP 解析失败：{}", parse_error);
            response.status = 500;
            response.set_content(
                soap_ns::build_fault(soap_ns::error_code::kInvalidArgs, parse_error),
                "text/xml; charset=\"utf-8\"");
            return;
        }

        actions_handled.fetch_add(1, std::memory_order_relaxed);

        ActionResult results;
        int error_code = soap_ns::error_code::kInvalidAction;
        std::string error_text;

        bool ok = false;
        if (parsed.service_type == description_ns::kAvTransportType) {
            ok = dispatch_av_transport(parsed, &results, &error_code, &error_text);
        } else if (parsed.service_type == description_ns::kRenderingControlType) {
            ok = dispatch_rendering_control(parsed, &results, &error_code, &error_text);
        } else if (parsed.service_type == description_ns::kConnectionManagerType) {
            ok = dispatch_connection_manager(parsed, &results, &error_code, &error_text);
        } else {
            error_text = "未知服务类型：" + parsed.service_type;
        }

        if (!ok) {
            AD_LOG_WARN("SOAP 动作失败 {}：{}", parsed.action_name, error_text);
            {
                std::lock_guard<std::mutex> lock(mutex);
                last_error = error_text;
            }
            // 注意 HTTP 状态码仍然是 500 + SOAP Fault，而不是 400 —— 这是
            // UPnP 的约定，手机端靠 Fault 里的 errorCode 区分原因。
            response.status = 500;
            response.set_content(soap_ns::build_fault(error_code, error_text),
                                 "text/xml; charset=\"utf-8\"");
            return;
        }

        response.set_content(
            soap_ns::build_action_response(parsed.service_type, parsed.action_name, results),
            "text/xml; charset=\"utf-8\"");
    }

    void handle_subscribe(const httplib::Request& request, httplib::Response& response) {
        const std::string callback = request.get_header_value("CALLBACK");
        const std::string sid = request.get_header_value("SID");

        if (callback.empty() && sid.empty()) {
            response.status = 412;   // Precondition Failed，UPnP 约定
            return;
        }

        // sid 非空表示续订，callback 非空表示新订阅。
        std::lock_guard<std::mutex> lock(mutex);

        if (!sid.empty()) {
            for (Subscription& subscription : subscriptions) {
                if (subscription.sid == sid) {
                    subscription.expires_at = std::chrono::steady_clock::now() +
                                              std::chrono::seconds(kDefaultSubscriptionSeconds);
                    response.set_header("SID", subscription.sid);
                    response.set_header("TIMEOUT", "Second-" + std::to_string(kDefaultSubscriptionSeconds));
                    return;
                }
            }
            response.status = 412;   // 续订一个不存在的 SID
            return;
        }

        // 新订阅：生成 SID。规范要求形如 "uuid:<uuid>"。
        Subscription subscription;
        subscription.sid = "uuid:" + common::generate_uuid_v4();
        subscription.callback_url = callback;
        subscription.expires_at = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(kDefaultSubscriptionSeconds);
        subscriptions.push_back(subscription);

        AD_LOG_DEBUG("收到 GENA 订阅：{}", callback);

        response.set_header("SID", subscription.sid);
        response.set_header("TIMEOUT", "Second-" + std::to_string(kDefaultSubscriptionSeconds));
    }

    void handle_unsubscribe(const httplib::Request& request, httplib::Response& response) {
        const std::string sid = request.get_header_value("SID");
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = subscriptions.begin(); it != subscriptions.end(); ++it) {
            if (it->sid == sid) {
                subscriptions.erase(it);
                return;
            }
        }
        response.status = 412;
    }

    void register_routes() {
        server.Get("/description.xml", [this](const httplib::Request&, httplib::Response& response) {
            handle_description(response);
        });

        // SCPDURL 用的是带冒号的 serviceId，作为路径段要能匹配上。
        server.Get(R"(/scpd/(.+))", [this](const httplib::Request& request, httplib::Response& response) {
            handle_scpd(request.matches[1], response);
        });

        server.Post("/control", [this](const httplib::Request& request, httplib::Response& response) {
            handle_control(request, response);
        });

        // 订阅与续订都走 SUBSCRIBE，取消走 UNSUBSCRIBE。
        //
        // cpp-httplib 只支持标准 HTTP 方法，SUBSCRIBE / UNSUBSCRIBE 属于
        // WebDAV/UPnP 的扩展方法，没有对应的注册接口。所以用 pre-routing
        // 处理器在路由之前拦截 —— 返回 Handled 表示已处理，库不会再往下走。
        server.set_pre_routing_handler(
            [this](const httplib::Request& request, httplib::Response& response) {
                if (request.path != "/event") {
                    return httplib::Server::HandlerResponse::Unhandled;
                }
                if (request.method == "SUBSCRIBE") {
                    handle_subscribe(request, response);
                    return httplib::Server::HandlerResponse::Handled;
                }
                if (request.method == "UNSUBSCRIBE") {
                    handle_unsubscribe(request, response);
                    return httplib::Server::HandlerResponse::Handled;
                }
                return httplib::Server::HandlerResponse::Unhandled;
            });

        // 手机有时会先 GET 一下事件地址探测可用性。
        server.Get("/event", [](const httplib::Request&, httplib::Response& response) {
            response.status = 405;
        });
    }
};

// ===========================================================================

DlnaRenderer::DlnaRenderer() : impl_(new Impl()) {}

DlnaRenderer::~DlnaRenderer() {
    stop();
}

bool DlnaRenderer::start(const DlnaConfig& config, std::string* out_error) {
    if (impl_->running.load()) {
        return true;
    }

    const std::string address = config.local_address.empty()
                                    ? common::primary_local_ipv4()
                                    : config.local_address;
    if (address.empty()) {
        const std::string message = "没有可用的局域网地址，DLNA 无法提供设备描述";
        if (out_error != nullptr) {
            *out_error = message;
        }
        AD_LOG_ERROR("{}", message);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->config = config;
        impl_->base_url = "http://" + address + ":" + std::to_string(config.http_port);
        impl_->last_error.clear();
    }

    impl_->register_routes();

    // 先 listen 探测端口能否绑定，失败时能立刻给出明确原因 ——
    // 49152 常被别的投屏软件占着。
    if (!impl_->server.bind_to_any_port("0.0.0.0", config.http_port)) {
        const std::string message =
            "DLNA 端口 " + std::to_string(config.http_port) + " 绑定失败，可能被占用。";
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->last_error = message;
        }
        if (out_error != nullptr) {
            *out_error = message;
        }
        AD_LOG_ERROR("{}", message);
        return false;
    }

    impl_->running.store(true);
    impl_->server_thread = std::thread([this]() {
        impl_->server.listen_after_bind();
    });

    AD_LOG_INFO("DLNA 设备描述已就绪：{}/description.xml", impl_->base_url);
    return true;
}

void DlnaRenderer::stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }

    impl_->server.stop();

    if (impl_->server_thread.joinable()) {
        impl_->server_thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->subscriptions.clear();
    }

    AD_LOG_INFO("DLNA 已停止");
}

bool DlnaRenderer::is_running() const {
    return impl_->running.load();
}

bool DlnaRenderer::set_device_name(const std::string& name, std::string* out_error) {
    const common::DeviceNameValidation validation = common::validate_device_name(name);
    if (!validation.valid) {
        if (out_error != nullptr) {
            *out_error = validation.reason;
        }
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->config.device_name = validation.normalized;
    // 不需要重启 HTTP 服务：描述是每次请求现生成的，下次手机来拉就是新名字。
    return true;
}

void DlnaRenderer::set_listener(IDlnaListener* listener) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->listener = listener;
}

std::string DlnaRenderer::description_url() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->base_url + "/description.xml";
}

uint16_t DlnaRenderer::port() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->config.http_port;
}

void DlnaRenderer::notify_transport_state(const std::string& state) {
    impl_->push_event(description_ns::kAvTransportType, {{"TransportState", state}});
}

void DlnaRenderer::notify_volume_changed(int volume) {
    const int clamped = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    impl_->push_event(description_ns::kRenderingControlType,
                      {{"Volume", std::to_string(clamped)}});
}

void DlnaRenderer::notify_mute_changed(bool mute) {
    impl_->push_event(description_ns::kRenderingControlType,
                      {{"Mute", mute ? "1" : "0"}});
}

void DlnaRenderer::notify_duration_changed(int64_t duration_ms) {
    impl_->push_event(description_ns::kAvTransportType,
                      {{"CurrentMediaDuration", soap_ns::format_duration(duration_ms)}});
}

uint64_t DlnaRenderer::action_count() const {
    return impl_->actions_handled.load(std::memory_order_relaxed);
}

std::string DlnaRenderer::last_error() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->last_error;
}

}  // namespace adisplay::dlna
