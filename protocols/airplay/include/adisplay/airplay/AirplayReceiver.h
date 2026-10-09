// ADisplay —— AirPlay 接收端（文档 3.1，批次 4）
//
// 职责边界：这个类只做协议。它不渲染、不解码、不广播。
//
//   * 控制通道（/info、/pair-setup、/pair-verify、/fp-setup、SETUP…）
//     由协议层在自己绑定的端口上提供。
//   * 解密后的音视频帧通过 IAirplayListener 交出去，由界面层/媒体管线处理。
//   * mDNS 广播仍归 DiscoveryService —— 它需要一个稳定的端口才能广播，
//     所以启动顺序必须是「先起接收端拿到真实端口，再广播」。见 AdEngine。
//
// 协议实现复用 UxPlay，理由见 cmake/UxPlayProtocol.cmake。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace adisplay::airplay {

struct AirplayConfig {
    // 显示名，即 iPhone「屏幕镜像」列表里看到的名字（文档 2.4）。
    std::string device_name;

    // MAC 风格的设备 ID（冒号分隔，大小写不敏感）。用于 _raop._tcp 的
    // 实例名前缀，也是 iOS 认设备身份的依据。
    std::string device_id;

    // 配对密钥的落盘路径。
    //
    // 协议层在这里存一对 Ed25519 密钥，公钥会广播出去。留空则每次启动都
    // 重新生成 —— 那意味着 iPhone 每次都要重新配对，所以生产路径上必须给。
    std::string key_file;

    // 控制通道端口。被占用时协议层会在传入值上更新为实际绑定的端口。
    uint16_t port = 7000;

    // 镜像视频的目标尺寸与帧率，作为建议值发给发送端。
    // 0 表示让协议层用默认值（1920x1080@60，最高 30fps）。
    int display_width = 0;
    int display_height = 0;
    int display_refresh_rate = 0;
    int display_max_fps = 0;
};

// 协议层的回调。
//
// 全部在工作线程上触发。调用方拿到指针的生存期只到这次调用结束 ——
// 需要留存内容就得自己拷走。
class IAirplayListener {
public:
    virtual ~IAirplayListener() = default;

    // 有设备连上控制通道 / 断开（文档 6.2 的连接确认就挂在这两个点上）。
    virtual void on_client_connected(const std::string& device_id,
                                     const std::string& model,
                                     const std::string& name) = 0;
    virtual void on_client_disconnected() = 0;

    // 镜像开始与结束。注意它和上面的连接不是一回事：设备可能连上控制通道
    // 却始终不开始镜像（例如只是从控制中心点开看了一眼）。
    virtual void on_mirror_started() = 0;
    virtual void on_mirror_stopped() = 0;

    // AirPlay 视频推送（文档 3.1.1 表格最后一行）：手机只给一个 HLS 地址，
    // 由接收端自己去拉。这条路径和 DLNA 一模一样 —— 拿到 URL 交给平台播放器。
    virtual void on_video_play(const std::string& url, float start_position) = 0;
    virtual void on_video_stop() = 0;

    // 镜像流的视频帧。数据是 AVCC 格式的 H.264/H.265（已解密），
    // 每个 NALU 前面是 4 字节长度前缀，不是 Annex B 的起始码。
    //
    // 第一帧通常是编码参数（H.264 的 SPS/PPS）—— 渲染端要先拿它建格式描述。
    // width/height 是发送端报来的画面尺寸，可能为 0（还没收到尺寸信息）；
    // pts_us 是显示时间戳（微秒），为 0 表示发送端没给。
    virtual void on_video_frame(const unsigned char* data, int size, bool is_h265,
                                uint32_t width, uint32_t height, int64_t pts_us) = 0;

    // 镜像流的音频帧。compression_type 是 ALAC / AAC-ELD 等的标识。
    virtual void on_audio_frame(const unsigned char* data, int size,
                                int compression_type) = 0;

    // 发送端在问「音量是多少」（协议把回放音量存在接收端）。
    virtual double on_volume_requested() = 0;
    virtual void on_volume_changed(int volume) = 0;
};

// 实现细节，定义在 .cpp 里。
//
// 放在命名空间作用域而不是类内部：协议层的回调是 .cpp 里的一组自由函数，
// 它们要解引用这个结构体 —— 嵌在类里就是私有类型，自由函数够不着。
struct AirplayReceiverImpl;

class AirplayReceiver {
public:
    AirplayReceiver();
    ~AirplayReceiver();

    AirplayReceiver(const AirplayReceiver&) = delete;
    AirplayReceiver& operator=(const AirplayReceiver&) = delete;

    // 启动控制通道并绑定端口。失败时 out_error 里是原因。
    bool start(const AirplayConfig& config, std::string* out_error);

    void stop();

    bool is_running() const;

    // 实际绑定的端口。要在 start() 成功之后取 —— 端口被占用时协议层
    // 会另选一个，广播必须用这个值。
    uint16_t port() const;

    // 协议层规范化后的设备 ID（小写、冒号分隔）。
    //
    // 广播必须用这个值，不能用我们配置里存的那份：协议层的 /info 应答
    // 是拿同一段原始字节自己格式化出来的，两边只要有一处大小写不同，
    // iOS 就会当成两台设备。
    std::string device_id() const;

    // 配对公钥（十六进制）。为空说明密钥还没就绪 —— 此时广播会缺 pk，
    // iOS 能看见设备但配不上对，所以调用方必须把这件事记进日志。
    std::string public_key() const;

    // 协议层的型号与版本号。广播的 model / srcvers 必须用这两个值 ——
    // /info 应答里的 model 与 sourceVersion 就是它们，差一个字 iOS 就会
    // 把同一台设备当成两台。照抄常量是不行的：上游升级换了值，抄的那份不会动。
    std::string model() const;
    std::string srcvers() const;

    void set_listener(IAirplayListener* listener);

    // 运行中更新「向发送端建议的显示尺寸」（画质档位，见 AdQualityPreset）。
    //
    // 协议层每次应答 /info 时都会读这几个值，所以手机**下次**连接就会按新尺寸
    // 编码，不需要重启接收端。当前这一路镜像已经按旧尺寸编码了，改不了。
    void set_display_size(int width, int height);

private:
    std::unique_ptr<AirplayReceiverImpl> impl_;
};

// 这份构建是否编进了 AirPlay 协议层。
//
// 当前 Android 不带它 —— APK 体积和交叉编译风险都要单独处理，
// 等桌面两端验证通过再接。界面层据此如实提示，而不是让用户对着
// 「屏幕镜像能看到但连不上」自己猜。
bool airplay_supported();

}  // namespace adisplay::airplay
