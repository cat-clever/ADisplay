// ADisplay —— C ABI 的实现（引擎门面）
//
// 这个文件是 adisplay.h 里每个函数的落地。它自己不做协议，只负责：
//   * 生命周期与状态机
//   * 配置的读入、校验、持久化
//   * 把内部 C++ 事件翻译成 C 回调
//
// 批次 0 阶段协议服务尚未接入，start() 只做端口占用探测与状态流转 ——
// 端口探测本身是文档 4.5 明确要求的功能（macOS 自带 AirPlay 接收器占 7000）。
#include <adisplay/adisplay.h>
#include <adisplay/version.h>   // 由 CMake 生成

#include <adisplay/airplay/AirplayReceiver.h>
#include <adisplay/common/Config.h>
#include <adisplay/discovery/DiscoveryService.h>
#include <adisplay/dlna/DlnaRenderer.h>
#include <chrono>

#include <adisplay/pipeline/AacEldConfig.h>
#include <adisplay/pipeline/AacEldDecoder.h>
#include <adisplay/pipeline/MediaRelay.h>
#include <adisplay/common/DeviceIdentity.h>
#include <adisplay/common/DeviceName.h>
#include <adisplay/common/Log.h>
#include <adisplay/common/NetUtil.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace {

// AdEngine 结构体与各 C 接口函数都定义在全局作用域，所以这两个别名
// 必须放在这个匿名命名空间里 —— 匿名命名空间的名字在全局可见，
// 漏掉哪一个，全局那边就会报 "use of undeclared identifier"。
namespace common = adisplay::common;
namespace discovery = adisplay::discovery;
namespace airplay = adisplay::airplay;
namespace dlna = adisplay::dlna;
namespace pipeline = adisplay::pipeline;

constexpr uint16_t kDefaultAirplayPort = 7000;
constexpr uint16_t kDefaultDlnaPort    = 49152;
constexpr uint16_t kDefaultCastpcPort  = 8765;

// 端口被占用时向上试探的次数。
constexpr int kPortProbeAttempts = 20;

// ---------------------------------------------------------------------------
// 配置文件的键名。改动会让老用户丢配置，别随便动。
// ---------------------------------------------------------------------------
constexpr const char* kKeyDeviceName     = "device.name";
constexpr const char* kKeyAirplayPort    = "network.airplay_port";
constexpr const char* kKeyDlnaPort       = "network.dlna_port";
constexpr const char* kKeyCastpcPort     = "network.castpc_port";
constexpr const char* kKeyEnableAirplay  = "service.airplay";
constexpr const char* kKeyEnableDlna     = "service.dlna";
constexpr const char* kKeyEnableCastpc   = "service.castpc";
constexpr const char* kKeyQualityPreset  = "video.quality_preset";
constexpr const char* kKeyRequireConfirm = "security.require_confirmation";
constexpr const char* kKeyLogLevel       = "log.level";

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::string to_std_string(const char* text) {
    if (text == nullptr) {
        return std::string();
    }
    return std::string(text);
}

// 把 std::string 写进调用方的缓冲区。
// 成功返回 AD_OK；缓冲区不够返回 AD_ERR_BUFFER_TOO_SMALL，
// 并（无论成败）通过 out_length 回填所需字节数（含结尾 '\0'）。
AdResult copy_to_buffer(const std::string& source, char* buffer,
                        std::size_t buffer_size, std::size_t* out_length) {
    const std::size_t needed = source.size() + 1u;
    if (out_length != nullptr) {
        *out_length = needed;
    }
    if (buffer == nullptr || buffer_size < needed) {
        return AD_ERR_BUFFER_TOO_SMALL;
    }
    if (!source.empty()) {
        std::memcpy(buffer, source.data(), source.size());
    }
    buffer[source.size()] = '\0';
    return AD_OK;
}

bool is_valid_log_level(int level) {
    return level >= static_cast<int>(common::LogLevel::Trace) &&
           level <= static_cast<int>(common::LogLevel::Off);
}

// 协议层用 ct 标记音频编码。8 是 AAC(-ELD)，也就是屏幕镜像的伴音；
// 2 是 ALAC，走的是音频模式那条路。
constexpr int kAacEldCompressionType = 8;

bool is_valid_quality_preset(int preset) {
    return preset >= static_cast<int>(AD_QUALITY_SMOOTH) &&
           preset <= static_cast<int>(AD_QUALITY_SHARP);
}

// 画质档位落到实处的杠杆：向发送端建议的显示尺寸。
//
// 手机把自己的屏幕缩放到这个尺寸再编码，投出来的视频就是这个分辨率。手机是
// 竖屏，所以**高度**才是起作用的那个数：建议 1920x1080 时手机量出来的是
// 498x1080（屏占比 0.46），在 1080p 屏上 1:1 刚好，放到 Retina 全屏就明显发虚。
//
// 宽度取 16:9 的标准值 —— 手机只把它当外框，竖屏用高度、横屏用宽度，两边都合适。
struct DisplaySuggestion {
    int width;
    int height;
};

DisplaySuggestion display_suggestion_for_preset(int preset) {
    switch (preset) {
        case AD_QUALITY_SMOOTH:
            // 与协议层默认一致。1080p 屏够用，代价最低。
            return DisplaySuggestion{1920, 1080};
        case AD_QUALITY_SHARP:
            // 桌面全屏看，按 Retina 的像素量给足。
            return DisplaySuggestion{3840, 2160};
        case AD_QUALITY_BALANCED:
        default:
            return DisplaySuggestion{2560, 1440};
    }
}

}  // namespace

// ===========================================================================
// 引擎内部结构
// ===========================================================================

// DLNA 的 C++ 回调要转成 C ABI 回调，这件事由 DlnaBridge 做。
// 它得读 AdEngine 的回调快照，所以只能在 AdEngine 之后定义 ——
// 这里先声明，成员用 unique_ptr 持有。
class DlnaBridge;
class AirplayBridge;

struct AdEngine {
    mutable std::mutex mutex;

    common::LogLevel log_level = common::LogLevel::Info;

    // 回调在锁外调用，避免用户代码回调进来时死锁。
    AdCallbacks callbacks{};
    void* user_data = nullptr;

    std::atomic<int> state{AD_STATE_STOPPED};

    std::string device_name;
    std::string config_path;
    std::string log_file_path;

    common::DeviceIdentity identity;
    common::Config persisted;

    uint16_t airplay_port = kDefaultAirplayPort;
    uint16_t dlna_port    = kDefaultDlnaPort;
    uint16_t castpc_port  = kDefaultCastpcPort;

    bool enable_airplay = true;
    bool enable_dlna    = true;
    bool enable_castpc  = true;

    int  quality_preset     = AD_QUALITY_BALANCED;
    bool require_confirmation = true;

    std::string last_error;

    // 服务发现：mDNS 广播 + SSDP 应答（文档 4.2）。
    // 用 unique_ptr 是因为 DiscoveryService 不可拷贝且构造较重，
    // 而 AdEngine 会用 new 直接分配。
    std::unique_ptr<discovery::DiscoveryService> discovery;

    // DLNA 渲染器：提供设备描述与 SOAP 控制端点（文档 3.2）。
    std::unique_ptr<dlna::DlnaRenderer> dlna_renderer;

    // 本地换封装中转（文档 4.1 的媒体管线）。平台播放器吃不下某些封装，
    // 典型是 HEVC-in-MPEG-TS —— Apple 的 HLS 规范要求 HEVC 用 fMP4，AVPlayer
    // 遇到 HEVC-in-TS 会丢掉视频轨（现象是进度条能拖、有声音、没画面）。
    //
    // 它只是按需介入：不是 HLS、或者拉下列表发现分片本来就是 fMP4，都会原样
    // 放行。所以留着它对别的片源没有代价。
    std::unique_ptr<pipeline::MediaRelay> media_relay;

    // DLNA 回调 -> C ABI 的桥，批次 3 接入。
    std::unique_ptr<DlnaBridge> dlna_bridge;

    // AirPlay 接收端（批次 4）。
    //
    // 和 DLNA 渲染器不同，它自己绑控制通道的端口，也是唯一知道配对公钥的地方
    // —— 广播要用的端口、设备 id、公钥都得从它这里取（见 start()）。
    std::unique_ptr<airplay::AirplayReceiver> airplay_receiver;

    // AirPlay 回调 -> C ABI 的桥。
    std::unique_ptr<AirplayBridge> airplay_bridge;

    // 单独一把锁保护 dlna_renderer 指针本身。不复用 mutex 是因为 GENA 推送
    // 会真的发 HTTP 请求、可能卡好几秒，而 mutex 被回调快照和配置读写共用，
    // 卡在那里会连累一大片不相干的调用。
    mutable std::mutex renderer_mutex;

    // 声明出来，让 unique_ptr 的析构点落在 DlnaBridge 定义之后。
    ~AdEngine();

    // 取出用户数据与回调的快照，供锁外调用。
    void notify_state_changed(int new_state) {
        AdCallbacks snapshot{};
        void* user = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = callbacks;
            user = user_data;
        }
        if (snapshot.on_state_changed != nullptr) {
            snapshot.on_state_changed(user, new_state);
        }
    }

    void notify_log(int level, const std::string& message) {
        AdCallbacks snapshot{};
        void* user = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = callbacks;
            user = user_data;
        }
        if (snapshot.on_log != nullptr) {
            snapshot.on_log(user, level, message.c_str());
        }
    }

    // ---- 给 DLNA 桥用的转发 -------------------------------------------------
    //
    // 和上面几个同一个模式：持锁取快照，锁外调用户代码。用户在回调里回头
    // 再调 ad_engine_* 是很自然的写法，持锁调用必然死锁。

    // peer_kind 由调用方给出：DLNA 只可能来自 Android，AirPlay 只可能来自
    // iOS。在这里写死成其中一种，界面层就会把 iPhone 显示成安卓设备。
    void notify_session_opened(uint32_t session_id, int stream_kind, int peer_kind) {
        AdCallbacks snapshot{};
        void* user = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = callbacks;
            user = user_data;
        }
        if (snapshot.on_session_opened == nullptr) {
            return;
        }
        // 字符串要活过回调，所以用局部的。DLNA 一行都没带发送端信息，
        // 给空串而不是 NULL —— 界面层直接 std::string(peer->display_name)
        // 是很自然的写法，NULL 会当场崩。
        const std::string display_name;
        const std::string address;
        AdPeerInfo peer{};
        peer.struct_size = static_cast<uint32_t>(sizeof(AdPeerInfo));
        peer.display_name = display_name.c_str();
        peer.address = address.c_str();
        peer.kind = peer_kind;
        snapshot.on_session_opened(user, session_id, &peer, stream_kind);
    }

    void notify_session_closed(uint32_t session_id, int reason) {
        AdCallbacks snapshot{};
        void* user = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = callbacks;
            user = user_data;
        }
        if (snapshot.on_session_closed != nullptr) {
            snapshot.on_session_closed(user, session_id, reason);
        }
    }

    void notify_media_url(uint32_t session_id, const std::string& url,
                          const std::string& mime_type) {
        AdCallbacks snapshot{};
        void* user = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = callbacks;
            user = user_data;
        }
        if (snapshot.on_media_url != nullptr) {
            snapshot.on_media_url(user, session_id, url.c_str(), mime_type.c_str());
        }
    }

    // 镜像视频帧。和别的转发同一个模式：持锁取快照，锁外调用户代码。
    //
    // data 只在回调期间有效 —— 界面层要留存必须自己拷走，这一点写在 adisplay.h 里。
    void notify_mirror_frame(uint32_t session_id, const unsigned char* data, int size,
                             bool is_h265, uint32_t width, uint32_t height, int64_t pts_us) {
        AdCallbacks snapshot{};
        void* user = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = callbacks;
            user = user_data;
        }
        if (snapshot.on_mirror_frame == nullptr) {
            return;
        }
        AdMirrorFrame frame{};
        frame.struct_size = static_cast<uint32_t>(sizeof(AdMirrorFrame));
        frame.session_id = session_id;
        frame.data = data;
        frame.size = size;
        frame.is_h265 = is_h265 ? 1 : 0;
        frame.width = width;
        frame.height = height;
        frame.pts_us = pts_us;
        snapshot.on_mirror_frame(user, &frame);
    }

    // 界面层是否要自己解镜像伴音（注册了压缩回调就表示要）。
    //
    // 有它才有可能「注册了就不解码」：核心在每帧进来时问一次，答案是要，
    // 就只转发压缩帧。Android 靠这条路避开把 FFmpeg 链进 APK。
    bool wants_compressed_mirror_audio() {
        std::lock_guard<std::mutex> lock(mutex);
        return callbacks.on_mirror_audio_frame != nullptr;
    }

    // 镜像伴音的**压缩**帧（AAC-ELD 裸帧）。只有界面层注册了对应回调时才走到。
    void notify_mirror_audio_frame(uint32_t session_id, const unsigned char* data, int size,
                                   uint32_t sample_rate, uint32_t channels, int64_t pts_us) {
        AdCallbacks snapshot{};
        void* user = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = callbacks;
            user = user_data;
        }
        if (snapshot.on_mirror_audio_frame == nullptr) {
            return;
        }
        AdMirrorAudioFrame frame{};
        frame.struct_size = static_cast<uint32_t>(sizeof(AdMirrorAudioFrame));
        frame.session_id = session_id;
        frame.data = data;
        frame.size = size;
        frame.sample_rate = sample_rate;
        frame.channels = channels;
        frame.pts_us = pts_us;
        frame.reserved = 0;
        snapshot.on_mirror_audio_frame(user, &frame);
    }

    // 镜像伴音。与视频是同一路 AirPlay 会话的两条流，所以共用会话号 ——
    // 界面层据此知道这段声音属于哪块画面。
    //
    // data 是**解码后**的交错 float32（LRLRLR…）。解码在核心里做是有意的：
    // AAC-ELD 在 Windows 的 Media Foundation 与 Android 的 MediaCodec 上都不
    // 保证支持，而核心已经有 FFmpeg —— 一份实现三端通用。这也正是 AdAudioFrame
    // 从一开始就按交错 float32 定义的原因。
    void notify_audio_frame(uint32_t session_id, const float* data, int frame_count,
                            uint32_t sample_rate, uint32_t channels, int64_t pts_us) {
        AdCallbacks snapshot{};
        void* user = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = callbacks;
            user = user_data;
        }
        if (snapshot.on_audio_frame == nullptr) {
            return;
        }
        AdAudioFrame frame{};
        frame.struct_size = static_cast<uint32_t>(sizeof(AdAudioFrame));
        frame.session_id = session_id;
        frame.pts_us = pts_us;
        frame.sample_rate = sample_rate;
        frame.channels = channels;
        frame.frame_count = static_cast<uint32_t>(frame_count);
        frame.data = data;
        frame.reserved = 0;
        snapshot.on_audio_frame(user, &frame);
    }

    void notify_playback_command(uint32_t session_id, int command, int64_t value) {
        AdCallbacks snapshot{};
        void* user = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            snapshot = callbacks;
            user = user_data;
        }
        if (snapshot.on_playback_command != nullptr) {
            snapshot.on_playback_command(user, session_id, command, value);
        }
    }

    // 推送 GENA 事件。持 renderer_mutex 是为了和 stop() 里的析构互斥 ——
    // 推送会阻塞，所以这把锁必须和 mutex 分开。
    void publish_playback_event(const std::string& transport_state, int64_t duration_ms,
                                int volume, bool mute_changed, bool muted) {
        std::lock_guard<std::mutex> lock(renderer_mutex);
        if (!dlna_renderer) {
            return;
        }
        if (!transport_state.empty()) {
            dlna_renderer->notify_transport_state(transport_state);
        }
        if (duration_ms >= 0) {
            dlna_renderer->notify_duration_changed(duration_ms);
        }
        if (volume >= 0) {
            dlna_renderer->notify_volume_changed(volume);
        }
        if (mute_changed) {
            dlna_renderer->notify_mute_changed(muted);
        }
    }

    // 状态迁移 + 通知。只在状态真的变了时才通知。
    void change_state(int new_state) {
        const int previous = state.exchange(new_state);
        if (previous != new_state) {
            notify_state_changed(new_state);
        }
    }

    void set_last_error(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        last_error = message;
    }

    // 把内存里的设置写回配置对象并落盘。
    AdResult persist() {
        common::Config snapshot;
        std::string path;
        {
            std::lock_guard<std::mutex> lock(mutex);
            identity.store(persisted);
            persisted.set_string(kKeyDeviceName, device_name);
            persisted.set_int(kKeyAirplayPort, airplay_port);
            persisted.set_int(kKeyDlnaPort, dlna_port);
            persisted.set_int(kKeyCastpcPort, castpc_port);
            persisted.set_bool(kKeyEnableAirplay, enable_airplay);
            persisted.set_bool(kKeyEnableDlna, enable_dlna);
            persisted.set_bool(kKeyEnableCastpc, enable_castpc);
            persisted.set_int(kKeyQualityPreset, quality_preset);
            persisted.set_bool(kKeyRequireConfirm, require_confirmation);
            persisted.set_int(kKeyLogLevel, static_cast<int>(log_level));
            snapshot = persisted;
            path = config_path;
        }

        if (path.empty()) {
            return AD_OK;   // 没配路径表示不落盘，不算失败
        }

        std::string error;
        if (!snapshot.save(path, &error)) {
            set_last_error(error);
            AD_LOG_ERROR("保存配置失败：{}", error);
            return AD_ERR_INTERNAL;
        }
        return AD_OK;
    }
};

// DlnaBridge 的定义必须落在 AdEngine 之后。
AdEngine::~AdEngine() = default;

// ---------------------------------------------------------------------------
// DLNA 回调 -> C ABI 的桥（批次 3）
//
// DlnaRenderer 讲的是一套 C++ 接口（IDlnaListener），界面层只认 adisplay.h。
// 这一层把两边对上，并缓存界面层回报的播放器状态。
//
// 状态为什么缓存在核心侧：手机的 GetTransportInfo / GetPositionInfo /
// GetMediaInfo / GetVolume 都是同步 SOAP 应答，跨语言回调要同步取值很别扭；
// 而且手机轮询很密，缓存下来最省事。缓存由 ad_engine_report_playback() 刷新。
// ---------------------------------------------------------------------------
class DlnaBridge final : public dlna::IDlnaListener {
public:
    explicit DlnaBridge(AdEngine* engine) : engine_(engine) {}

    // ---- 手机推来一个新地址 -------------------------------------------------
    void on_set_uri(const std::string& url, const std::string& metadata) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            url_ = url;
            metadata_ = metadata;
            // 换媒体要把进度清掉，不然新视频一上来进度条还是上一首的位置。
            position_ms_ = -1;
            duration_ms_ = -1;
            transport_ = AD_TRANSPORT_TRANSITIONING;
        }

        // 一次推送算一次会话。旧的先按「被新会话抢占」关掉：部分 App 会先推
        // 一个占位地址再推真的，不关的话界面层会同时开着两个播放器。
        const uint32_t previous = session_id_.exchange(0);
        if (previous != 0) {
            engine_->notify_session_closed(previous, AD_CLOSE_REPLACED);
        }
        const uint32_t session = next_session_id_.fetch_add(1);
        session_id_.store(session);

        engine_->notify_session_opened(session, AD_STREAM_MEDIA_URL, AD_PEER_ANDROID);

        // 交给界面层之前先过一道本地中转。这一步只做字符串拼接、不发网络请求
        // —— 它在 SOAP 的 SetAVTransportURI 应答路径上，在这里拉远端内容会让
        // 手机等不到应答而判超时。要不要真换封装，等播放器来拉播放列表时再判。
        std::string playback_url = url;
        if (engine_->media_relay) {
            playback_url = engine_->media_relay->resolve_for_playback(url);
            if (playback_url != url) {
                AD_LOG_INFO("该片源可能需要在本地换封装，已改走中转：{}", playback_url);
            }
        }

        // mime_type 留空：准确类型在 DIDL-Lite 的 protocolInfo 里，那要再解一层
        // 转义过的 XML；平台播放器自己会嗅探容器，给空比给错更诚实。
        engine_->notify_media_url(session, playback_url, std::string());
    }

    // ---- 手机的控制意图：核心不执行，转给界面层的播放器 ----------------------
    void on_play() override { send_command(AD_CMD_PLAY, 0); }
    void on_pause() override { send_command(AD_CMD_PAUSE, 0); }
    void on_stop() override { send_command(AD_CMD_STOP, 0); }
    void on_seek(int64_t target_ms) override { send_command(AD_CMD_SEEK, target_ms); }
    void on_set_volume(int volume) override { send_command(AD_CMD_SET_VOLUME, volume); }
    void on_set_mute(bool mute) override { send_command(AD_CMD_SET_MUTE, mute ? 1 : 0); }

    // ---- 手机的同步查询：直接从缓存答 ----------------------------------------
    int64_t current_position_ms() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return position_ms_;
    }
    int64_t media_duration_ms() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return duration_ms_;
    }
    std::string current_transport_state() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return upnp_state_locked();
    }
    int current_volume() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return volume_;
    }
    bool current_mute() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return muted_;
    }

    // ---- 界面层回报播放器状态 ------------------------------------------------
    AdResult report(const AdPlaybackStatus& status) {
        const uint32_t session = session_id_.load();
        if (session == 0 || status.session_id != session) {
            return AD_ERR_NOT_FOUND;
        }

        std::string state_event;
        int64_t duration_event = -1;
        int volume_event = -1;
        bool mute_event = false;
        bool mute_changed = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            const int wanted = clamp_transport(status.transport_state);
            if (wanted != transport_) {
                transport_ = wanted;
                state_event = upnp_state_locked();
            }
            if (status.position_ms >= 0) {
                position_ms_ = status.position_ms;
            }
            if (status.duration_ms >= 0 && status.duration_ms != duration_ms_) {
                duration_ms_ = status.duration_ms;
                duration_event = duration_ms_;
            }
            // 用 -1 而不是 0 表示「这项没变」：0 是合法值（音量 0、未静音），
            // 拿 0 当不变会把真实状态冲掉。
            if (status.volume >= 0) {
                const int clamped = status.volume > 100 ? 100 : status.volume;
                if (clamped != volume_) {
                    volume_ = clamped;
                    volume_event = clamped;
                }
            }
            if (status.muted >= 0) {
                const bool value = status.muted != 0;
                if (value != muted_) {
                    muted_ = value;
                    mute_event = value;
                    mute_changed = true;
                }
            }
        }

        // 推送会发 HTTP 请求，绝不能在持 mutex_ 时做 —— 那会把所有
        // Get*Info 一起拖住（它们要拿同一把锁）。
        if (!state_event.empty() || duration_event >= 0 || volume_event >= 0 || mute_changed) {
            engine_->publish_playback_event(state_event, duration_event, volume_event,
                                            mute_changed, mute_event);
        }
        return AD_OK;
    }

    // 停服时清干净，避免下次 start 之后残留上一次的会话与状态。
    void reset() {
        session_id_.store(0);
        std::lock_guard<std::mutex> lock(mutex_);
        url_.clear();
        metadata_.clear();
        position_ms_ = -1;
        duration_ms_ = -1;
        transport_ = AD_TRANSPORT_NO_MEDIA_PRESENT;
        volume_ = 100;
        muted_ = false;
    }

private:
    void send_command(int command, int64_t value) {
        const uint32_t session = session_id_.load();
        if (session == 0) {
            // 还没推过媒体。手机不该显示这些按钮；真收到了也不该把一条
            // 没有会话的命令丢给界面层。
            AD_LOG_WARN("收到播放控制但当前没有媒体会话，已忽略（command={}）", command);
            return;
        }
        engine_->notify_playback_command(session, command, value);
    }

    // 界面层给了不认识的状态值时按「停着」处理，不要把越界值透给手机 ——
    // 手机端是按字串精确匹配的，收到没见过的值可能直接判定设备异常。
    static int clamp_transport(int value) {
        switch (value) {
            case AD_TRANSPORT_NO_MEDIA_PRESENT:
            case AD_TRANSPORT_STOPPED:
            case AD_TRANSPORT_PLAYING:
            case AD_TRANSPORT_PAUSED:
            case AD_TRANSPORT_TRANSITIONING:
                return value;
            default:
                return AD_TRANSPORT_STOPPED;
        }
    }

    // 手机端会精确匹配这些字串（UPnP AVTransport 规范），值取自
    // DlnaRenderer.h 里的 transport_state 常量，不要自己拼。
    std::string upnp_state_locked() const {
        switch (transport_) {
            case AD_TRANSPORT_PLAYING:       return dlna::transport_state::kPlaying;
            case AD_TRANSPORT_PAUSED:        return dlna::transport_state::kPaused;
            case AD_TRANSPORT_TRANSITIONING: return dlna::transport_state::kTransitioning;
            case AD_TRANSPORT_STOPPED:       return dlna::transport_state::kStopped;
            default:                         return dlna::transport_state::kNoMedia;
        }
    }

    AdEngine* engine_ = nullptr;

    mutable std::mutex mutex_;
    std::string url_;
    std::string metadata_;
    int64_t position_ms_ = -1;
    int64_t duration_ms_ = -1;
    int transport_ = AD_TRANSPORT_NO_MEDIA_PRESENT;
    // 与 DlnaRenderer 在没有 listener 时的取值保持一致（GetVolume 默认 100）。
    int volume_ = 100;
    bool muted_ = false;

    // 0 表示「还没有媒体会话」。命令与状态回报都按它过滤。
    std::atomic<uint32_t> session_id_{0};
    std::atomic<uint32_t> next_session_id_{1};
};

// AirPlay 回调 -> C ABI 的桥（批次 4）。
//
// 和 DlnaBridge 一样，核心自己不播放：
//   * AirPlay 视频推送给的是一条 HLS 地址，交给界面层的平台播放器去拉；
//   * 镜像流的音视频帧原样转出去，由渲染层处理。
//
// 镜像与视频推送共用一个会话号。对用户来说「一次投屏」就是一次会话 ——
// iPhone 先开镜像、再从某个 App 里推个视频，是同一台设备接二连三的动作，
// 界面上不该冒出两条并行记录。
class AirplayBridge final : public airplay::IAirplayListener {
public:
    explicit AirplayBridge(AdEngine* engine) : engine_(engine) {}

    void on_client_connected(const std::string& device_id, const std::string& model,
                             const std::string& name) override {
        std::lock_guard<std::mutex> lock(mutex_);
        device_id_ = device_id;
        model_ = model;
        name_ = name;
    }

    void on_client_disconnected() override {
        close_session(AD_CLOSE_USER_REQUEST);
    }

    void on_mirror_started() override {
        open_session(AD_STREAM_MIRROR_VIDEO);
    }

    void on_mirror_stopped() override {
        // 镜像结束不等于设备断开：iPhone 常常停掉镜像之后仍然连着，
        // 接着还能再推一个视频。所以这里只结束会话，不假设设备走了。
        close_session(AD_CLOSE_USER_REQUEST);
    }

    void on_video_play(const std::string& url, float start_position) override {
        (void) start_position;
        if (url.empty()) {
            AD_LOG_WARN("AirPlay 视频推送没带地址，起播不了");
            return;
        }
        const uint32_t session = open_session(AD_STREAM_MEDIA_URL);

        // 与 DLNA 同一条判断：有些片源平台播放器吃不下（典型是
        // HEVC-in-MPEG-TS），需要在本地换个封装再交给它。
        std::string playback_url = url;
        if (engine_->media_relay) {
            playback_url = engine_->media_relay->resolve_for_playback(url);
            if (playback_url != url) {
                AD_LOG_INFO("该片源可能需要在本地换封装，已改走中转：{}", playback_url);
            }
        }
        if (session != 0) {
            engine_->notify_media_url(session, playback_url, std::string());
        }
    }

    void on_video_stop() override {
        close_session(AD_CLOSE_USER_REQUEST);
    }

    void on_video_frame(const unsigned char* data, int size, bool is_h265,
                        uint32_t width, uint32_t height, int64_t pts_us) override {
        // 帧的到达与计数由 AirplayReceiver 记日志（节流也在那边），
        // 这里只做一件事：交给界面层去解码显示。
        //
        // 部分 iOS 版本不发 mirror_video_running，上来直接送帧 ——
        // 那就在第一帧时把会话开出来。
        const uint32_t session = session_id_.load() != 0
                                     ? session_id_.load()
                                     : open_session(AD_STREAM_MIRROR_VIDEO);
        if (session == 0) {
            return;
        }
        engine_->notify_mirror_frame(session, data, size, is_h265, width, height, pts_us);
    }

    // 镜像伴音（AAC-ELD）的解码器，以及几个只用于日志的标记。
    std::unique_ptr<pipeline::AacEldDecoder> audio_decoder_;
    std::atomic<bool> audio_decoder_failed{false};
    std::atomic<uint64_t> audio_frames_decoded{0};
    std::atomic<uint64_t> audio_frames_failed{0};

    // 伴音这条路的耗时拆分。CPU 高了必须先知道是「解码」还是「交给平台播放」——
    // 这两段的归属完全不同：解码在核心里，播放是回调进界面层再进系统音频。
    // 采样工具对实时音频线程的归属不可靠（它记录的是采样命中数，不是占用），
    // 所以这里直接计时，落在日志里。
    std::atomic<uint64_t> audio_decode_nanos{0};
    std::atomic<uint64_t> audio_notify_nanos{0};
    std::atomic<uint64_t> audio_timed_frames{0};
    std::atomic<int64_t> audio_timing_log_ms{0};

    void on_audio_frame(const unsigned char* data, int size, int compression_type) override {
        if (data == nullptr || size <= 0) {
            return;
        }
        // 协议层用 ct 标记音频编码：8 是 AAC(-ELD)，2 是 ALAC（音频模式那条路）。
        // 屏幕镜像的伴音只有 AAC-ELD 这一种，其余的这里不管。
        if (compression_type != kAacEldCompressionType) {
            return;
        }
        const uint32_t session = session_id_.load() != 0
                                     ? session_id_.load()
                                     : open_session(AD_STREAM_MIRROR_VIDEO);
        if (session == 0) {
            return;
        }

        // 界面层注册了压缩回调，就说明它自己会解（Android 的 MediaCodec 认
        // AAC-ELD，而把 FFmpeg 链进 APK 会让包大出上百兆）。这时核心只转发，
        // 一个字节都不解 —— 连解码器都不必创建。
        if (engine_->wants_compressed_mirror_audio()) {
            engine_->notify_mirror_audio_frame(
                session, data, size,
                static_cast<uint32_t>(pipeline::kAirplayMirrorAudioSampleRate),
                static_cast<uint32_t>(pipeline::kAirplayMirrorAudioChannels),
                0);
            return;
        }

        if (audio_decoder_failed.load() || !ensure_audio_decoder()) {
            return;
        }

        std::vector<float> pcm;
        int frames = 0;
        const auto decode_started = std::chrono::steady_clock::now();
        const bool decoded = audio_decoder_->decode(data, size, &pcm, &frames);
        const auto decode_finished = std::chrono::steady_clock::now();
        audio_decode_nanos.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(decode_finished - decode_started)
                .count()));
        if (!decoded) {
            // 单帧解不出来不致命：丢一帧十几毫秒，听感上是一声极短的静音，
            // 比为此中断整条流好得多。只在第一帧失败时报一次，避免刷屏。
            if (audio_frames_failed.fetch_add(1) == 0) {
                AD_LOG_WARN("AirPlay 镜像伴音：首帧解码失败，继续尝试后续帧");
            }
            return;
        }
        audio_frames_decoded.fetch_add(1);

        // 时间戳给 0：协议层交下来的音频帧不带可达的本地时间（视频那条回调会
        // 单独给 ntp_time_local，音频这条没有）。伴音按到达顺序播即可 —— 它与
        // 画面同源，十几毫秒的帧长本身就把节奏定死了。
        const auto notify_started = std::chrono::steady_clock::now();
        engine_->notify_audio_frame(
            session, pcm.data(), frames,
            static_cast<uint32_t>(pipeline::kAirplayMirrorAudioSampleRate),
            static_cast<uint32_t>(pipeline::kAirplayMirrorAudioChannels),
            0);
        const auto notify_finished = std::chrono::steady_clock::now();
        audio_notify_nanos.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(notify_finished - notify_started)
                .count()));
        audio_timed_frames.fetch_add(1);
        log_audio_timing_if_due();
    }

    // 每 5 秒一行：解码与「交给平台播放」各花多少毫秒。CPU 高了要看的正是这两半。
    void log_audio_timing_if_due() {
        // 只用来算间隔，所以取 steady_clock 自己的计数即可，不必换算成挂钟时间。
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        int64_t previous = audio_timing_log_ms.load();
        if (previous != 0 && now - previous < 5000) {
            return;
        }
        if (!audio_timing_log_ms.compare_exchange_strong(previous, now)) {
            return;
        }
        const uint64_t counted = audio_timed_frames.load();
        if (counted == 0) {
            return;
        }
        const double decode_ms =
            static_cast<double>(audio_decode_nanos.load()) / static_cast<double>(counted) / 1e6;
        const double notify_ms =
            static_cast<double>(audio_notify_nanos.load()) / static_cast<double>(counted) / 1e6;
        AD_LOG_INFO("AirPlay 镜像伴音：{} 帧，解码每帧 {:.3f} 毫秒，交给平台每帧 {:.3f} 毫秒",
                    counted, decode_ms, notify_ms);
    }

    // 解码器按需创建：不投声音的会话不该白白开一个。失败只报一次 ——
    // 每帧都报会把日志刷满，而原因始终是同一个。
    bool ensure_audio_decoder() {
        if (audio_decoder_ != nullptr) {
            return true;
        }
        auto decoder = std::make_unique<pipeline::AacEldDecoder>();
        std::string error;
        if (!decoder->init(pipeline::kAirplayMirrorAudioSampleRate,
                           pipeline::kAirplayMirrorAudioChannels, &error)) {
            AD_LOG_WARN("AirPlay 镜像伴音：解码器起不来 —— {}（这一路会没有声音）", error);
            audio_decoder_failed.store(true);
            return false;
        }
        AD_LOG_INFO("AirPlay 镜像伴音：解码器已就绪（AAC-ELD {} Hz / {} 声道）",
                    pipeline::kAirplayMirrorAudioSampleRate,
                    pipeline::kAirplayMirrorAudioChannels);
        audio_decoder_ = std::move(decoder);
        return true;
    }

    double on_volume_requested() override {
        // 协议层问「音量是多少」时要立刻作答，等不了界面层。返回 1.0（满音量）：
        // 报 0 会被发送端当成静音而改变它的行为。
        return 1.0;
    }

    void on_volume_changed(int volume) override {
        (void) volume;
    }

    // 停服时清干净，避免下次 start 之后残留上一次的会话。
    void reset() {
        close_session(AD_CLOSE_INTERNAL_ERROR);
        session_id_.store(0);
        std::lock_guard<std::mutex> lock(mutex_);
        device_id_.clear();
        model_.clear();
        name_.clear();
    }

private:
    // 已经有会话就复用。返回 0 只在这条消息不该起会话时出现。
    uint32_t open_session(int stream_kind) {
        const uint32_t existing = session_id_.load();
        if (existing != 0) {
            return existing;
        }
        const uint32_t session = next_session_id_.fetch_add(1);
        session_id_.store(session);
        engine_->notify_session_opened(session, stream_kind, AD_PEER_IOS);
        return session;
    }

    void close_session(int reason) {
        const uint32_t previous = session_id_.exchange(0);
        if (previous != 0) {
            engine_->notify_session_closed(previous, reason);
        }
    }

    AdEngine* engine_ = nullptr;

    mutable std::mutex mutex_;
    std::string device_id_;
    std::string model_;
    std::string name_;

    // 0 表示当前没有 AirPlay 会话。
    std::atomic<uint32_t> session_id_{0};
    std::atomic<uint32_t> next_session_id_{1};
};

// ===========================================================================
// 版本与工具
// ===========================================================================

const char* AD_CALL ad_version_string(void) {
    return ADISPLAY_VERSION;
}

uint32_t AD_CALL ad_abi_version(void) {
    return AD_ABI_VERSION;
}

const char* AD_CALL ad_result_string(AdResult result) {
    switch (result) {
        case AD_OK:                    return "ok";
        case AD_ERR_INVALID_ARG:       return "invalid argument";
        case AD_ERR_NOT_INITIALIZED:   return "engine not initialized";
        case AD_ERR_ALREADY_RUNNING:   return "already running";
        case AD_ERR_NOT_RUNNING:       return "not running";
        case AD_ERR_PORT_IN_USE:       return "port in use";
        case AD_ERR_PERMISSION_DENIED: return "permission denied";
        case AD_ERR_NETWORK:           return "network error";
        case AD_ERR_UNSUPPORTED:       return "unsupported";
        case AD_ERR_BUFFER_TOO_SMALL:  return "buffer too small";
        case AD_ERR_NOT_FOUND:         return "not found";
        case AD_ERR_INTERNAL:          return "internal error";
    }
    return "unknown";
}

// 用 malloc 分配，保证 Windows 上跨 DLL 边界 free 也不会出问题。
void AD_CALL ad_string_free(char* str) {
    std::free(str);
}

// ===========================================================================
// 默认配置
// ===========================================================================

AdResult AD_CALL ad_engine_get_default_config(AdConfig* out_config) {
    if (out_config == nullptr) {
        return AD_ERR_INVALID_ARG;
    }

    AdConfig config;
    std::memset(&config, 0, sizeof(config));

    config.struct_size   = static_cast<uint32_t>(sizeof(AdConfig));
    config.abi_version   = AD_ABI_VERSION;

    config.device_name   = nullptr;   // 由核心取平台默认名
    config.airplay_port  = kDefaultAirplayPort;
    config.dlna_port     = kDefaultDlnaPort;
    config.castpc_port   = kDefaultCastpcPort;

    config.enable_airplay = 1;
    config.enable_dlna    = 1;
    config.enable_castpc  = 1;

    config.quality_preset    = AD_QUALITY_BALANCED;
    config.require_confirmation = 1;
    config.log_level         = static_cast<int>(common::LogLevel::Info);

    config.log_file_path     = nullptr;
    config.config_file_path  = nullptr;

    config.bind_interfaces      = nullptr;
    config.bind_interface_count = 0;

    *out_config = config;
    return AD_OK;
}

// ===========================================================================
// 生命周期
// ===========================================================================

AdResult AD_CALL ad_engine_create(const AdConfig* config, AdEngine** out_engine) {
    if (out_engine == nullptr) {
        return AD_ERR_INVALID_ARG;
    }
    *out_engine = nullptr;

    // 调用方若给了配置，先校验结构体自描述字段，防止版本错配。
    if (config != nullptr) {
        if (config->struct_size < sizeof(AdConfig)) {
            return AD_ERR_INVALID_ARG;
        }
        if (config->abi_version != AD_ABI_VERSION) {
            return AD_ERR_INVALID_ARG;
        }
        if (config->log_level < 0 || !is_valid_log_level(config->log_level)) {
            return AD_ERR_INVALID_ARG;
        }
        if (config->quality_preset < 0 || !is_valid_quality_preset(config->quality_preset)) {
            return AD_ERR_INVALID_ARG;
        }
    }

    AdConfig effective;
    ad_engine_get_default_config(&effective);
    if (config != nullptr) {
        effective = *config;
        // 端口为 0 表示「用默认值」。
        if (effective.airplay_port == 0) {
            effective.airplay_port = kDefaultAirplayPort;
        }
        if (effective.dlna_port == 0) {
            effective.dlna_port = kDefaultDlnaPort;
        }
        if (effective.castpc_port == 0) {
            effective.castpc_port = kDefaultCastpcPort;
        }
    }

    std::unique_ptr<AdEngine> engine(new (std::nothrow) AdEngine());
    if (!engine) {
        return AD_ERR_INTERNAL;
    }

    engine->log_level = static_cast<common::LogLevel>(effective.log_level);
    engine->config_path = to_std_string(effective.config_file_path);
    engine->log_file_path = to_std_string(effective.log_file_path);
    if (engine->config_path.empty()) {
        engine->config_path = common::Config::default_config_path();
    }
    if (engine->log_file_path.empty()) {
        engine->log_file_path = common::Config::default_log_path();
    }

    // 先按调用方给的默认值铺一遍，再用磁盘上的配置覆盖 ——
    // 磁盘配置优先，这样重启后用户改过的名字、端口还在。
    engine->airplay_port = effective.airplay_port;
    engine->dlna_port    = effective.dlna_port;
    engine->castpc_port  = effective.castpc_port;
    engine->enable_airplay = effective.enable_airplay != 0;
    engine->enable_dlna    = effective.enable_dlna != 0;
    engine->enable_castpc  = effective.enable_castpc != 0;
    engine->quality_preset = effective.quality_preset;
    engine->require_confirmation = effective.require_confirmation != 0;

    std::string load_error;
    engine->persisted = common::Config::load(engine->config_path, &load_error);

    // 日志先起来，后面所有步骤都要能留下痕迹。
    common::Log::init(engine->log_level, engine->log_file_path);

    if (!load_error.empty()) {
        AD_LOG_WARN("读取配置有问题：{}", load_error);
        // 配置读坏了不让程序起不来，用默认值继续，由用户去修或重存。
        engine->persisted = common::Config();
    }

    // 标识必须持久化：改名、重启、升级都不能变，否则手机端会当成新设备
    // 要求重新配对（文档 2.4）。
    engine->identity = common::DeviceIdentity::load_or_create(engine->persisted);

    const std::string stored_name = engine->persisted.get_string(
        kKeyDeviceName, std::string());
    if (!stored_name.empty()) {
        common::DeviceNameValidation validation = common::validate_device_name(stored_name);
        if (validation.valid) {
            engine->device_name = validation.normalized;
        }
    }
    if (engine->device_name.empty()) {
        // 没有可用配置时，用调用方指定的名字，再退回平台默认名。
        const std::string requested = to_std_string(effective.device_name);
        if (!requested.empty()) {
            common::DeviceNameValidation validation = common::validate_device_name(requested);
            if (validation.valid) {
                engine->device_name = validation.normalized;
            }
        }
        if (engine->device_name.empty()) {
            engine->device_name = common::sanitize_device_name(common::default_device_name());
        }
    }

    // 磁盘上的值覆盖调用方给的默认值。
    engine->airplay_port = static_cast<uint16_t>(
        engine->persisted.get_int(kKeyAirplayPort, engine->airplay_port));
    engine->dlna_port = static_cast<uint16_t>(
        engine->persisted.get_int(kKeyDlnaPort, engine->dlna_port));
    engine->castpc_port = static_cast<uint16_t>(
        engine->persisted.get_int(kKeyCastpcPort, engine->castpc_port));
    engine->enable_airplay = engine->persisted.get_bool(kKeyEnableAirplay, engine->enable_airplay);
    engine->enable_dlna    = engine->persisted.get_bool(kKeyEnableDlna, engine->enable_dlna);
    engine->enable_castpc  = engine->persisted.get_bool(kKeyEnableCastpc, engine->enable_castpc);
    engine->quality_preset = static_cast<int>(
        engine->persisted.get_int(kKeyQualityPreset, engine->quality_preset));
    engine->require_confirmation = engine->persisted.get_bool(
        kKeyRequireConfirm, engine->require_confirmation);

    // 首次运行时 deviceid / uuid 是新生成的，必须立刻落盘。
    //
    // load_or_create 只在内存里生成，真正写盘要等 persist()，而 persist()
    // 原先只在改名时才调用 —— 用户不改名字的话，配置永远不会被写，
    // 下次启动又生成一套新标识。手机端会把它当成另一台设备要求重新配对，
    // 直接违反文档 2.4 的「标识不变」。
    if (!engine->persisted.has(common::kConfigKeyDeviceId)) {
        if (engine->persist() != AD_OK) {
            AD_LOG_WARN("设备标识落盘失败，下次启动可能会变化");
        }
    }

    // ---- 把核心日志转发给界面层 ------------------------------------------
    //
    // 不挂这个 sink 的话，核心产生的所有日志（端口冲突、mDNS 注册结果、
    // SSDP 在哪些网卡上广播）都到不了界面 —— AdEngine::notify_log 写了
    // 却从来没人调用，因为 AD_LOG_* 走的是 spdlog，不会自动触发它。
    //
    // 后果很具体：用户报「手机搜不到设备」时，界面上只有界面自己写的那一条
    // 「已就绪」，而真正能定位问题的信息一条都看不到。
    //
    // 捕获裸指针是安全的：销毁时先摘 sink 再 delete，见 ad_engine_destroy。
    AdEngine* const raw_engine = engine.get();
    common::Log::set_sink([raw_engine](common::LogLevel level, std::string_view message) {
        raw_engine->notify_log(static_cast<int>(level), std::string(message));
    });

    AD_LOG_INFO("ADisplay 核心已创建，版本 {}，设备名「{}」，标识 {}",
                ADISPLAY_VERSION, engine->device_name, engine->identity.device_id());

    *out_engine = engine.release();
    return AD_OK;
}

void AD_CALL ad_engine_destroy(AdEngine* engine) {
    if (engine == nullptr) {
        return;
    }

    ad_engine_stop(engine);

    AD_LOG_INFO("ADisplay 核心已销毁");

    // 必须先摘掉日志 sink 再销毁引擎：sink 里捕获的是 engine 的裸指针，
    // 顺序反了的话，之后任何一条日志都会写到已释放的内存上。
    common::Log::set_sink(nullptr);

    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        engine->callbacks = AdCallbacks{};
        engine->user_data = nullptr;
    }

    delete engine;

    // 引擎没了，日志系统也一并收掉。多引擎场景下这会关掉别人的日志，
    // 但当前设计只允许一个引擎实例。
    common::Log::shutdown();
}

AdResult AD_CALL ad_engine_start(AdEngine* engine) {
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }

    const int current = engine->state.load();
    if (current == AD_STATE_RUNNING || current == AD_STATE_STREAMING ||
        current == AD_STATE_STARTING) {
        return AD_ERR_ALREADY_RUNNING;
    }

    // 上一次若以失败告终（AD_STATE_ERROR），这里允许直接重试。
    engine->change_state(AD_STATE_STARTING);

    // ---- 端口占用探测 ----------------------------------------------------
    // 文档 4.5：macOS 12+ 系统自带「AirPlay 接收器」会占用 5000 与 7000 端口，
    // 被占用时提示用户关闭它，或改用其他端口。这里采取自动换端口的策略，
    // 并把这件事明确写进日志。
    uint16_t airplay_port = engine->airplay_port;
    uint16_t dlna_port    = engine->dlna_port;
    uint16_t castpc_port  = engine->castpc_port;

    if (engine->enable_airplay) {
        const int probe = common::probe_tcp_port(airplay_port);
        if (probe == 5) {   // AD_ERR_PORT_IN_USE
            const uint16_t fallback = common::find_available_tcp_port(
                static_cast<uint16_t>(airplay_port + 1), kPortProbeAttempts);
            if (fallback == 0) {
                const std::string message =
                    "AirPlay 端口 " + std::to_string(airplay_port) +
                    " 被占用，且找不到可用替代端口。"
                    "macOS 上通常是系统自带的「AirPlay 接收器」占着 —— "
                    "请在「系统设置 → 通用 → 隔空投送与接力」里关闭它。";
                engine->set_last_error(message);
                AD_LOG_ERROR("{}", message);
                engine->change_state(AD_STATE_ERROR);
                return AD_ERR_PORT_IN_USE;
            }
            AD_LOG_WARN("AirPlay 端口 {} 被占用（macOS 上通常是系统自带 AirPlay 接收器），"
                        "已自动改用 {}", airplay_port, fallback);
            airplay_port = fallback;
        } else if (probe == 6) {
            const std::string message =
                "AirPlay 端口 " + std::to_string(airplay_port) + " 权限不足。";
            engine->set_last_error(message);
            AD_LOG_ERROR("{}", message);
            engine->change_state(AD_STATE_ERROR);
            return AD_ERR_PERMISSION_DENIED;
        }
    }

    if (engine->enable_dlna) {
        if (common::probe_tcp_port(dlna_port) == 5) {
            const uint16_t fallback = common::find_available_tcp_port(
                static_cast<uint16_t>(dlna_port + 1), kPortProbeAttempts);
            if (fallback != 0) {
                AD_LOG_WARN("DLNA 端口 {} 被占用，已自动改用 {}", dlna_port, fallback);
                dlna_port = fallback;
            }
        }
    }

    if (engine->enable_castpc) {
        if (common::probe_tcp_port(castpc_port) == 5) {
            const uint16_t fallback = common::find_available_tcp_port(
                static_cast<uint16_t>(castpc_port + 1), kPortProbeAttempts);
            if (fallback != 0) {
                AD_LOG_WARN("自研协议端口 {} 被占用，已自动改用 {}", castpc_port, fallback);
                castpc_port = fallback;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        engine->airplay_port = airplay_port;
        engine->dlna_port    = dlna_port;
        engine->castpc_port  = castpc_port;
    }

    // ---- 启动 DLNA 的 HTTP 服务 ------------------------------------------
    // 顺序很重要：SSDP 通告里的 LOCATION 指向 /description.xml，那个地址
    // 必须与这里实际监听的端口完全一致。手机拿到 LOCATION 后会立刻去拉，
    // 拉不到就直接把设备从列表里去掉 —— 表现是「设备出现后立刻消失」。
    // 所以先起 HTTP 服务确定端口，再用同一个端口值去做 SSDP 通告。
    if (engine->enable_dlna) {
        uint16_t dlna_port_in_use = common::find_available_tcp_port(
            engine->dlna_port, kPortProbeAttempts);
        if (dlna_port_in_use == 0) {
            const std::string message =
                "DLNA 找不到可用端口（从 " + std::to_string(engine->dlna_port) + " 起）";
            engine->set_last_error(message);
            AD_LOG_ERROR("{}", message);
            engine->change_state(AD_STATE_ERROR);
            return AD_ERR_PORT_IN_USE;
        }
        if (dlna_port_in_use != engine->dlna_port) {
            AD_LOG_WARN("DLNA 端口 {} 被占用，改用 {}",
                        engine->dlna_port, dlna_port_in_use);
        }
        dlna_port = dlna_port_in_use;

        dlna::DlnaConfig dlna_config;
        dlna_config.device_name = engine->device_name;
        dlna_config.uuid = engine->identity.uuid();
        dlna_config.http_port = dlna_port;
        // 这里不设 SERVER 头：那是 SSDP 通告的字段（由 DiscoveryService 负责），
        // DLNA 的设备描述里没有它。

        engine->dlna_renderer = std::make_unique<dlna::DlnaRenderer>();

        std::string dlna_error;
        if (!engine->dlna_renderer->start(dlna_config, &dlna_error)) {
            const std::string message = "DLNA 启动失败：" + dlna_error;
            engine->set_last_error(message);
            AD_LOG_ERROR("{}", message);
            engine->dlna_renderer.reset();
            engine->change_state(AD_STATE_ERROR);
            return AD_ERR_NETWORK;
        }
        // 把播放意图与播放状态接给界面层（批次 3）。
        //
        // 核心自己不播放：DLNA 给的是一条 URL，解码与渲染交给各平台自带的
        // 播放器（macOS AVPlayer / Windows MediaPlayerElement / 电视端
        // ExoPlayer）。这一层只做两件事 —— 把手机的控制意图转成 C 回调
        // 送出去，以及缓存界面层回报的状态供手机的 Get*Info 同步应答。
        engine->dlna_bridge = std::make_unique<DlnaBridge>(engine);
        engine->dlna_renderer->set_listener(engine->dlna_bridge.get());

        // 中转起不来不该让整个接收服务失败：绝大多数片源不需要它，只是那一类
        // 片源会没画面。所以这里只记警告，把原因留在日志里。
        engine->media_relay = std::make_unique<pipeline::MediaRelay>();
        std::string relay_error;
        if (!engine->media_relay->start(&relay_error)) {
            AD_LOG_WARN("本地换封装中转启动失败，部分片源可能没有画面：{}", relay_error);
            engine->media_relay.reset();
        }

        dlna_port = engine->dlna_renderer->port();
    }

    // ---- 启动 AirPlay 接收端 ----------------------------------------------
    //
    // 必须排在广播之前，两个原因：
    //
    //  1. 广播只是「看不看得见」，控制通道才是「连不连得上」。少了后者，
    //     iPhone 会在「屏幕镜像」里列出本机，点下去却毫无反应 —— 这比
    //     设备根本不出现更让人困惑。
    //  2. 端口、设备 id、配对公钥都要用它给出的那份。端口是它自己绑的
    //     （被占用时协议层另选一个），而设备 id 它会在 /info 里按自己的
    //     规则格式化一遍；广播若用配置里存的原串，大小写一差，iOS 就会
    //     当成两台设备。
    //
    // 它起不来不算整个服务失败：DLNA 那一路通常还是好的。但这时**必须**
    // 停止广播 AirPlay —— 广播一个没人应答的服务，用户看到的就是
    // 「看得见、点不动」。
    bool airplay_ready = false;
    std::string airplay_device_id = engine->identity.device_id();
    std::string airplay_public_key;
    if (engine->enable_airplay && !airplay::airplay_supported()) {
        // 这份构建没编入协议层（目前是 Android）。如实说出来 —— 否则用户
        // 面对的是「镜像列表里能看到设备、点了没反应」，而真正的原因在别处。
        AD_LOG_WARN("这份构建没有编入 AirPlay 协议层，本次不广播 AirPlay"
                    "（DLNA 与自研协议不受影响）");
    } else if (engine->enable_airplay) {
        airplay::AirplayConfig airplay_config;
        airplay_config.device_name = engine->device_name;
        airplay_config.device_id = engine->identity.device_id();
        airplay_config.port = airplay_port;

        // 画质档位决定建议给手机的显示尺寸。不填的话协议层用它的默认值
        // （1920x1080），手机就把屏幕缩到 1080 高再编码 —— Retina 上全屏看
        // 会明显发虚。这里把它接上。
        const DisplaySuggestion suggestion = display_suggestion_for_preset(engine->quality_preset);
        airplay_config.display_width = suggestion.width;
        airplay_config.display_height = suggestion.height;

        // 配对密钥要和配置放在一起。它必须落盘：每次启动重新生成的话，
        // iPhone 会把本机当成新设备，每次投屏都要重新配对一遍。
        std::filesystem::path config_file(engine->config_path);
        if (config_file.has_parent_path()) {
            airplay_config.key_file =
                (config_file.parent_path() / "airplay-pairing.key").string();
        } else {
            airplay_config.key_file = "airplay-pairing.key";
        }

        // 这一段包了 try：它要碰文件系统（配对密钥落盘）与协议层的初始化，
        // 而那些都可能抛 C++ 异常。ad_engine_start 是 C ABI 入口，异常一旦
        // 越过这个边界就是直接 terminate —— 用户的感受是「点开关程序就没了」，
        // 而且不留任何日志。宁可 AirPlay 起不来、DLNA 照常，也不要这样退。
        try {
            engine->airplay_receiver = std::make_unique<airplay::AirplayReceiver>();
            engine->airplay_bridge = std::make_unique<AirplayBridge>(engine);
            engine->airplay_receiver->set_listener(engine->airplay_bridge.get());

            std::string airplay_error;
            if (engine->airplay_receiver->start(airplay_config, &airplay_error)) {
                airplay_ready = true;
                airplay_port = engine->airplay_receiver->port();
                airplay_device_id = engine->airplay_receiver->device_id();
                airplay_public_key = engine->airplay_receiver->public_key();
                AD_LOG_INFO("AirPlay 接收端已就绪，控制通道端口 {}", airplay_port);
            } else {
                engine->airplay_receiver.reset();
                engine->airplay_bridge.reset();
                const std::string message =
                    "AirPlay 接收端启动失败，本次不广播 AirPlay：" + airplay_error;
                AD_LOG_ERROR("{}", message);
                engine->set_last_error(message);
            }
        } catch (const std::exception& ex) {
            engine->airplay_receiver.reset();
            engine->airplay_bridge.reset();
            const std::string message =
                std::string("AirPlay 接收端启动时抛出异常，本次不广播 AirPlay：") + ex.what();
            AD_LOG_ERROR("{}", message);
            engine->set_last_error(message);
        } catch (...) {
            engine->airplay_receiver.reset();
            engine->airplay_bridge.reset();
            const std::string message =
                "AirPlay 接收端启动时抛出未知异常，本次不广播 AirPlay";
            AD_LOG_ERROR("{}", message);
            engine->set_last_error(message);
        }
    }

    // ---- 启动服务发现 ----------------------------------------------------
    // 端口都确认可用了，开始广播。手机在投屏列表里能不能看到我们，
    // 完全取决于这一步有没有成功。
    //
    // 这里不加锁：start / stop / set_device_name 都由界面线程调用，
    // 三者不会并发。回调可能从 discovery 的工作线程触发，但那些回调
    // 不碰 AdEngine 的成员。
    {
        discovery::DiscoveryConfig discovery_config;
        discovery_config.device_name = engine->device_name;
        // 用接收端规范化后的设备 id（小写）。协议层的 /info 是拿同一段
        // 原始字节按自己的规则转出来的，广播用另一个写法就会被当成两台设备。
        discovery_config.device_id = airplay_ready ? airplay_device_id
                                                   : engine->identity.device_id();
        discovery_config.uuid = engine->identity.uuid();
        discovery_config.airplay_port = airplay_port;
        discovery_config.dlna_port = dlna_port;
        discovery_config.castpc_port = castpc_port;
        // 配对公钥：缺了它 iPhone 能看见我们却配不上对。
        discovery_config.airplay_pk = airplay_public_key;
        // 型号与版本同样取自协议层 —— /info 应答里的 model 与 sourceVersion
        // 就是这两个值，广播跟着用同一份才不会被当成两台设备。
        if (airplay_ready) {
            discovery_config.airplay_model = engine->airplay_receiver->model();
            discovery_config.airplay_srcvers = engine->airplay_receiver->srcvers();
        }
        // 只在接收端真的起来了才广播。这条是与「看得见、点不动」直接相关的
        // 一处：广播了却没有东西应答，用户无从判断问题出在哪一边。
        discovery_config.enable_airplay = airplay_ready;
        discovery_config.enable_dlna = engine->enable_dlna;
        discovery_config.enable_castpc = engine->enable_castpc;
        // SERVER 头按 UPnP 规范拼全：<OS>/<版本> UPnP/1.0 <产品>/<版本>。
        // 只写后半截的话，部分客户端解析失败会直接跳过这个设备。
        discovery_config.server_header = std::string(common::operating_system_name())
                                             + " UPnP/1.0 ADisplay/" + ADISPLAY_VERSION;

        engine->discovery = std::make_unique<discovery::DiscoveryService>();

        std::string discovery_error;
        if (!engine->discovery->start(discovery_config, &discovery_error)) {
            const std::string message = "广播启动失败：" + discovery_error;
            engine->set_last_error(message);
            AD_LOG_ERROR("{}", message);
            engine->discovery.reset();
            engine->change_state(AD_STATE_ERROR);
            return AD_ERR_NETWORK;
        }
        // 部分协议失败不算致命 —— 比如 AirPlay 的 mDNS 没注册上，
        // DLNA 通常还是能用的。记下来让界面能提示，但不阻塞启动。
        if (!discovery_error.empty()) {
            engine->set_last_error(discovery_error);
        }
    }

    // ---- 状态流转 --------------------------------------------------------
    const std::string local_ip = common::primary_local_ipv4();
    AD_LOG_INFO("接收服务已启动：设备名「{}」{}，AirPlay {} / DLNA {} / 自研 {}",
                engine->device_name,
                local_ip.empty() ? "" : ("，本机地址 " + local_ip),
                engine->enable_airplay ? std::to_string(airplay_port) : std::string("关闭"),
                engine->enable_dlna ? std::to_string(dlna_port) : std::string("关闭"),
                engine->enable_castpc ? std::to_string(castpc_port) : std::string("关闭"));

    engine->change_state(AD_STATE_RUNNING);
    return AD_OK;
}

void AD_CALL ad_engine_stop(AdEngine* engine) {
    if (engine == nullptr) {
        return;
    }

    const int current = engine->state.load();
    if (current == AD_STATE_STOPPED) {
        return;   // 没启动过，空操作
    }

    engine->change_state(AD_STATE_STOPPING);

    if (engine->discovery) {
        // stop 里会发出 mDNS 的 goodbye 与 SSDP 的 byebye，
        // 手机端列表里会立刻看到设备消失，而不是等超时。
        // 顺序上先停广播再停 HTTP：反过来手机可能还在拿着 LOCATION 来拉描述。
        engine->discovery->stop();
        engine->discovery.reset();
    }

    // 顺序与启动相反：先让广播 goodbye，再拆控制通道 —— 反过来手机可能
    // 还拿着刚看到的名字来连一个已经不存在的端口。
    if (engine->airplay_receiver) {
        // 与 DLNA 同一套路：先摘监听器，再停服务。停的过程中还可能有回调
        // 在路上，桥先没了就会踩空。
        engine->airplay_receiver->set_listener(nullptr);
        engine->airplay_receiver->stop();
        engine->airplay_receiver.reset();
    }
    if (engine->airplay_bridge) {
        engine->airplay_bridge->reset();
        engine->airplay_bridge.reset();
    }

    {
        // 先摘 listener 再停渲染器：停的过程中可能还有回调在路上，
        // 桥先没了就会踩空。renderer_mutex 同时挡住在途的 GENA 推送。
        std::lock_guard<std::mutex> lock(engine->renderer_mutex);
        if (engine->dlna_renderer) {
            engine->dlna_renderer->set_listener(nullptr);
            engine->dlna_renderer->stop();
            engine->dlna_renderer.reset();
        }
    }
    if (engine->dlna_bridge) {
        engine->dlna_bridge->reset();
        engine->dlna_bridge.reset();
    }

    // 放在渲染器拆完之后：上面 dlna_renderer->stop() 已经 join 掉 HTTP 线程，
    // 此刻不会再有 on_set_uri 在跑，所以可以安全地释放中转。
    if (engine->media_relay) {
        engine->media_relay->stop();
        engine->media_relay.reset();
    }

    engine->change_state(AD_STATE_STOPPED);
    AD_LOG_INFO("接收服务已停止");
}

AdResult AD_CALL ad_engine_get_state(AdEngine* engine, int* out_state) {
    if (engine == nullptr || out_state == nullptr) {
        return AD_ERR_INVALID_ARG;
    }
    *out_state = engine->state.load();
    return AD_OK;
}

AdResult AD_CALL ad_engine_set_callbacks(AdEngine* engine,
                                         const AdCallbacks* callbacks,
                                         void* user_data) {
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }

    if (callbacks != nullptr) {
        if (callbacks->struct_size < sizeof(AdCallbacks)) {
            return AD_ERR_INVALID_ARG;
        }
        const int current_state = engine->state.load();
        if (current_state == AD_STATE_RUNNING || current_state == AD_STATE_STREAMING ||
            current_state == AD_STATE_STARTING || current_state == AD_STATE_STOPPING) {
            // 运行中换回调会让在途事件落到一半旧一半新的处理函数上。
            // AD_STATE_ERROR 不在此列 —— 启动失败后界面正需要换回调重试。
            return AD_ERR_ALREADY_RUNNING;
        }
    }

    std::lock_guard<std::mutex> lock(engine->mutex);
    engine->callbacks = (callbacks != nullptr) ? *callbacks : AdCallbacks{};
    engine->user_data = (callbacks != nullptr) ? user_data : nullptr;
    return AD_OK;
}

AdResult AD_CALL ad_engine_get_last_error(AdEngine* engine,
                                          char* buf, std::size_t buf_size,
                                          std::size_t* out_len) {
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }
    std::string message;
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        message = engine->last_error;
    }
    if (message.empty()) {
        if (out_len != nullptr) {
            *out_len = 0;
        }
        if (buf != nullptr && buf_size > 0) {
            buf[0] = '\0';
        }
        return AD_OK;
    }
    return copy_to_buffer(message, buf, buf_size, out_len);
}

// ===========================================================================
// 设备名称与配置
// ===========================================================================

AdResult AD_CALL ad_device_name_validate(const char* utf8_name, char* reason_buf,
                                         std::size_t reason_size) {
    if (utf8_name == nullptr) {
        if (reason_buf != nullptr && reason_size > 0) {
            const char* message = "设备名称不能为空";
            const std::size_t length = std::strlen(message);
            const std::size_t copy_length = (length < reason_size - 1) ? length : (reason_size - 1);
            std::memcpy(reason_buf, message, copy_length);
            reason_buf[copy_length] = '\0';
        }
        return AD_ERR_INVALID_ARG;
    }

    const common::DeviceNameValidation validation = common::validate_device_name(utf8_name);
    if (validation.valid) {
        if (reason_buf != nullptr && reason_size > 0) {
            reason_buf[0] = '\0';
        }
        return AD_OK;
    }

    if (reason_buf != nullptr && reason_size > 0) {
        const std::size_t length = validation.reason.size();
        const std::size_t copy_length = (length < reason_size - 1u) ? length : (reason_size - 1u);
        if (copy_length > 0) {
            std::memcpy(reason_buf, validation.reason.data(), copy_length);
        }
        reason_buf[copy_length] = '\0';
    }
    return AD_ERR_INVALID_ARG;
}

AdResult AD_CALL ad_engine_set_device_name(AdEngine* engine, const char* utf8_name) {
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }

    const common::DeviceNameValidation validation =
        common::validate_device_name(to_std_string(utf8_name));
    if (!validation.valid) {
        engine->set_last_error(validation.reason);
        AD_LOG_WARN("设备名称不合规：{}", validation.reason);
        return AD_ERR_INVALID_ARG;
    }

    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        if (engine->device_name == validation.normalized) {
            return AD_OK;   // 没变，省掉一次 mDNS 重注册
        }
        engine->device_name = validation.normalized;
    }

    // 文档 2.4「即时生效」：注销并重新注册 mDNS / SSDP，无需重启软件，
    // deviceid 与已配对记录保持不变 —— 已连接过的手机不需要重新配对。
    AD_LOG_INFO("设备名称已改为「{}」，正在重新注册广播", validation.normalized);

    if (engine->discovery) {
        std::string discovery_error;
        if (!engine->discovery->set_device_name(validation.normalized, &discovery_error)) {
            // 改名失败不回滚内存里的名字：名称本身是合法的，失败的只是广播。
            // 下次启停服务时会用新名字重新注册。
            AD_LOG_WARN("广播改名失败：{}", discovery_error);
        }
    }

    if (engine->dlna_renderer) {
        std::string dlna_error;
        if (!engine->dlna_renderer->set_device_name(validation.normalized, &dlna_error)) {
            AD_LOG_WARN("DLNA 改名失败：{}", dlna_error);
        }
        // 设备描述是每次请求现生成的，不需要重启 HTTP 服务 ——
        // 手机下次来拉 description.xml 就是新名字。
    }

    return engine->persist();
}

AdResult AD_CALL ad_engine_get_device_name(AdEngine* engine,
                                           char* buf, std::size_t buf_size,
                                           std::size_t* out_len) {
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }
    std::string name;
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        name = engine->device_name;
    }
    return copy_to_buffer(name, buf, buf_size, out_len);
}

AdResult AD_CALL ad_engine_set_quality_preset(AdEngine* engine, int preset) {
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }
    if (!is_valid_quality_preset(preset)) {
        return AD_ERR_INVALID_ARG;
    }
    airplay::AirplayReceiver* receiver = nullptr;
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        engine->quality_preset = preset;
        receiver = engine->airplay_receiver.get();
    }

    // 让正在跑的接收端也用上。协议层在每次 /info 应答时读这几个值，所以手机
    // 下次连接就会按新尺寸编码 —— 不必重启服务，也不必断开当前这一路。
    const DisplaySuggestion suggestion = display_suggestion_for_preset(preset);
    if (receiver != nullptr) {
        receiver->set_display_size(suggestion.width, suggestion.height);
    }

    AD_LOG_INFO("画质档位已切换：建议显示尺寸 {}x{}（手机下次连接时生效）",
                suggestion.width, suggestion.height);
    return engine->persist();
}

AdResult AD_CALL ad_engine_get_quality_preset(AdEngine* engine, int* out_preset) {
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }
    if (out_preset == nullptr) {
        return AD_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(engine->mutex);
    *out_preset = engine->quality_preset;
    return AD_OK;
}

AdResult AD_CALL ad_engine_save_config(AdEngine* engine) {
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }
    return engine->persist();
}

// ===========================================================================
// 连接确认与会话
//
// 批次 0 尚未接入协议，没有真实的请求与会话。这些函数先返回 AD_ERR_NOT_FOUND，
// 由批次 1 / 2 / 4 填入实现 —— 返回 AD_ERR_UNSUPPORTED 会让界面误以为
// 平台不支持该功能。
// ===========================================================================

AdResult AD_CALL ad_engine_respond_connect_request(AdEngine* engine, uint32_t request_id,
                                                   int allow, int remember) {
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }
    (void)allow;
    (void)remember;
    (void)request_id;
    return AD_ERR_NOT_FOUND;
}

AdResult AD_CALL ad_engine_disconnect_session(AdEngine* engine, uint32_t session_id) {
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }
    (void)session_id;
    return AD_ERR_NOT_FOUND;
}

AdResult AD_CALL ad_engine_report_playback(AdEngine* engine, const AdPlaybackStatus* status) {
    if (engine == nullptr || status == nullptr) {
        return AD_ERR_INVALID_ARG;
    }
    if (status->struct_size != sizeof(AdPlaybackStatus) ||
        status->abi_version != AD_ABI_VERSION) {
        return AD_ERR_INVALID_ARG;
    }
    if (!engine->dlna_bridge) {
        return AD_ERR_NOT_RUNNING;
    }
    return engine->dlna_bridge->report(*status);
}

AdResult AD_CALL ad_engine_get_session_count(AdEngine* engine, uint32_t* out_count) {
    if (engine == nullptr || out_count == nullptr) {
        return AD_ERR_INVALID_ARG;
    }
    *out_count = 0;
    return AD_OK;
}

AdResult AD_CALL ad_engine_get_sessions(AdEngine* engine, AdPeerInfo* peers,
                                        uint32_t capacity, uint32_t* out_count) {
    if (engine == nullptr || out_count == nullptr) {
        return AD_ERR_INVALID_ARG;
    }
    (void)peers;
    (void)capacity;
    *out_count = 0;
    return AD_OK;
}

// ===========================================================================
// 本地信息
// ===========================================================================

AdResult AD_CALL ad_engine_get_local_addresses(AdEngine* engine, char* buf,
                                               std::size_t buf_size,
                                               std::size_t* out_len) {
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }

    // 只列出适合广播的地址：回环和虚拟网卡对用户没有意义（文档 6.3）。
    std::string joined;
    for (const common::NetworkAddress& address : common::broadcastable_addresses()) {
        if (!joined.empty()) {
            joined.push_back('\n');
        }
        joined += address.address;
    }

    return copy_to_buffer(joined, buf, buf_size, out_len);
}

AdResult AD_CALL ad_engine_get_device_id(AdEngine* engine, char* buf,
                                         std::size_t buf_size,
                                         std::size_t* out_len) {
    if (engine == nullptr) {
        return AD_ERR_NOT_INITIALIZED;
    }
    std::string identifier;
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        identifier = engine->identity.device_id();
    }
    return copy_to_buffer(identifier, buf, buf_size, out_len);
}
