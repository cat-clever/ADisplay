// ADisplay —— DLNA 渲染器（DMR）
//
// 文档 3.2：Android 上绝大多数视频 App 的「投屏」按钮走的都是 DLNA/UPnP，
// 我们的角色是 DMR（Digital Media Renderer，数字媒体渲染器）。
//
// 需要实现三个服务（缺一个手机端就可能判定设备不可用）：
//   AVTransport       播放控制：SetAVTransportURI / Play / Pause / Stop /
//                     Seek / GetPositionInfo / GetTransportInfo
//   RenderingControl  音量：SetVolume / GetVolume / SetMute
//   ConnectionManager 能力声明：GetProtocolInfo
//
// 两个 HTTP 端点：
//   GET  /description.xml   设备描述。手机发现我们之后第一件事就是拉它，
//                           拿不到就直接把设备从列表里去掉。
//   POST /control           所有 SOAP 动作都打到这里，按 SOAPAction 头分发。
//   SUBSCRIBE /control      事件订阅。手机靠它同步播放状态（进度条、音量）。
//
// 播放本身不在这一层：DlnaRenderer 只把「要播什么」「怎么控制」转给
// 监听者，真正的解码渲染媒体管线在 core/pipeline 下（批次 3）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace adisplay::dlna {

// 传输状态，取值与 UPnP 的 AVTransport 规范一致。
// 手机端会拿它来显示播放/暂停按钮的状态，字串必须精确匹配。
namespace transport_state {
inline constexpr const char* kStopped = "STOPPED";
inline constexpr const char* kPlaying = "PLAYING";
inline constexpr const char* kPaused = "PAUSED_PLAYBACK";
inline constexpr const char* kTransitioning = "TRANSITIONING";
inline constexpr const char* kNoMedia = "NO_MEDIA_PRESENT";
}  // namespace transport_state

// 手机端发起的一件控制意图。由 DlnaRenderer 转给媒体管线执行。
class IDlnaListener {
public:
    virtual ~IDlnaListener() = default;

    // 手机要求播放这个地址。metadata 是 DIDL-Lite 的 XML，
    // 里面带标题、封面、时长等信息，可以拿来显示"正在播放：xxx"。
    virtual void on_set_uri(const std::string& url, const std::string& metadata) = 0;

    virtual void on_play() = 0;
    virtual void on_pause() = 0;
    virtual void on_stop() = 0;

    // 跳转。target_ms 是绝对位置（毫秒）。
    virtual void on_seek(int64_t target_ms) = 0;

    // 音量与静音。volume 取值 0..100。
    virtual void on_set_volume(int volume) = 0;
    virtual void on_set_mute(bool mute) = 0;

    // 手机在查询当前状态，需要同步返回。
    // 这几个会被 SOAP 的 Get*Info 动作直接调用，所以必须立刻给出答案 ——
    // 不能异步，否则手机会认为设备无响应并断开。
    virtual int64_t current_position_ms() const = 0;
    virtual int64_t media_duration_ms() const = 0;
    virtual std::string current_transport_state() const = 0;
    virtual int current_volume() const = 0;
    virtual bool current_mute() const = 0;
};

struct DlnaConfig {
    // 显示名，手机端列表里看到的就是它（文档 2.4）。
    std::string device_name;

    // 设备唯一标识。改名不影响它（文档 2.4「标识不变」）。
    std::string uuid;

    // 设备描述里显示的品牌与型号。自用项目，填成能认出来的名字即可。
    std::string manufacturer = "ADisplay";
    std::string model_name = "ADisplay Receiver";
    std::string model_number = "1";

    // HTTP 服务监听端口。设备描述与 SOAP 都走它。
    uint16_t http_port = 49152;

    // 用于拼 LOCATION 的地址。留空表示由实现取本机局域网地址。
    std::string local_address;
};

class DlnaRenderer {
public:
    DlnaRenderer();
    ~DlnaRenderer();

    DlnaRenderer(const DlnaRenderer&) = delete;
    DlnaRenderer& operator=(const DlnaRenderer&) = delete;

    // 启动 HTTP 服务。端口被占用时返回 false。
    //
    // 注意这只负责 HTTP 部分：让手机能在 SSDP 结果里看到我们，
    // 是 DiscoveryService 的事（批次 1 已完成），两者要配合使用 ——
    // LOCATION 指向的地址必须正是这里监听的地址与端口。
    bool start(const DlnaConfig& config, std::string* out_error);

    void stop();

    bool is_running() const;

    // 改名即时生效：设备描述里的 friendlyName 立刻变。
    // 不需要重启 HTTP 服务，也不用重新 SSDP 注册（显示名已在批次 1 的
    // SSDP 通告里改过了），但手机如果已经拉过描述，要等它下次刷新。
    bool set_device_name(const std::string& name, std::string* out_error);

    // 媒体管线接进来，接收控制意图。
    void set_listener(IDlnaListener* listener);

    // 设备描述 XML 的完整 URL，供 DiscoveryService 填进 SSDP 的 LOCATION。
    std::string description_url() const;

    uint16_t port() const;

    // ---- 由媒体管线回推状态，会触发 GENA 事件 ----------------------------
    //
    // 手机端的进度条与音量滑块靠这些事件更新。只在状态真的变了时才推送 ——
    // 每秒都推会让手机端不停重绘。
    void notify_transport_state(const std::string& state);
    void notify_volume_changed(int volume);
    void notify_mute_changed(bool mute);
    void notify_duration_changed(int64_t duration_ms);

    // 排障用：收到的 SOAP 动作总数与最近一次失败原因。
    uint64_t action_count() const;
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace adisplay::dlna
