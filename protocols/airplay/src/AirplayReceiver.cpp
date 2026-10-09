// ADisplay —— AirPlay 接收端实现（接口说明见 AirplayReceiver.h）

#include <adisplay/airplay/AirplayReceiver.h>

#include <adisplay/common/Log.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

// 这份构建是否带协议层。不带时下面的 raop 相关代码整段不参与编译，
// 但类的接口保持一致 —— 界面层不需要为「有没有 AirPlay」写两套调用。
#ifndef ADISPLAY_HAVE_AIRPLAY_RECEIVER
#  define ADISPLAY_HAVE_AIRPLAY_RECEIVER 0
#endif

#if ADISPLAY_HAVE_AIRPLAY_RECEIVER
extern "C" {
#include "dnssd.h"
#include "logger.h"
#include "raop.h"
#include "stream.h"
}
#include "DnssdShim.h"
#endif

namespace adisplay::airplay {
namespace {

// 帧率日志的间隔。镜像期间每秒几十帧，逐帧记会淹没日志窗口，
// 但完全没有记录又无法回答「画面卡住是没数据还是渲染的问题」。
constexpr int64_t kFrameLogIntervalMs = 5000;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// "AA:BB:CC:DD:EE:FF" -> 6 字节。协议层要原始字节，自己拿去格式化。
bool parse_mac_address(const std::string& text, unsigned char* out, int out_size) {
    if (out_size != 6) {
        return false;
    }
    int index = 0;
    int nibble = -1;
    unsigned char byte = 0;

    for (char ch : text) {
        int value = -1;
        if (ch >= '0' && ch <= '9') {
            value = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            value = 10 + (ch - 'a');
        } else if (ch >= 'A' && ch <= 'F') {
            value = 10 + (ch - 'A');
        } else {
            continue;   // 冒号和其它分隔符一律跳过
        }

        if (nibble < 0) {
            byte = static_cast<unsigned char>(value << 4);
            nibble = 0;
        } else {
            byte = static_cast<unsigned char>(byte | value);
            if (index >= out_size) {
                return false;
            }
            out[index++] = byte;
            nibble = -1;
        }
    }
    return index == out_size;
}

}  // namespace

bool airplay_supported() {
#if ADISPLAY_HAVE_AIRPLAY_RECEIVER
    return true;
#else
    return false;
#endif
}

#if ADISPLAY_HAVE_AIRPLAY_RECEIVER

struct AirplayReceiverImpl {
    mutable std::mutex mutex;

    raop_t* raop = nullptr;
    dnssd_t* dnssd = nullptr;

    bool running = false;
    uint16_t port = 0;

    std::string device_id_text;
    std::string public_key_text;
    std::string model_text;
    std::string srcvers_text;

    IAirplayListener* listener = nullptr;

    // 回放音量。协议把它存在接收端，手机来问的时候要立刻给出答案。
    // 用 atomic 是因为它会被 HTTP 工作线程读取。
    std::atomic<int> volume{100};

    // 镜像状态与帧计数，只用于日志。
    std::atomic<bool> mirroring{false};
    std::atomic<uint64_t> video_frames{0};
    std::atomic<uint64_t> audio_frames{0};
    std::atomic<int64_t> last_frame_log_ms{0};
    std::atomic<bool> logged_first_video_frame{false};

    IAirplayListener* listener_snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return listener;
    }

    // 交出去的帧可能是几十 KB，逐帧打印在真机上就是灾难，所以压成
    // 「首帧一次 + 之后每 5 秒一行计数」。
    void note_video_frame(int size, bool is_h265) {
        video_frames.fetch_add(1);
        if (!logged_first_video_frame.exchange(true)) {
            AD_LOG_INFO("AirPlay 镜像视频流已建立，首帧 {} 字节（编码 {}）",
                        size, is_h265 ? "H.265" : "H.264");
            last_frame_log_ms.store(now_ms());
            return;
        }
        const int64_t now = now_ms();
        // 不能是 const：compare_exchange_strong 在失败时会把实际值写回来，
        // 参数是非 const 引用。
        int64_t previous = last_frame_log_ms.load();
        if (now - previous < kFrameLogIntervalMs) {
            return;
        }
        // 多个线程可能同时越过上面的判断，用比较交换认领这一次打印，
        // 否则同一秒会刷出好几行。
        if (!last_frame_log_ms.compare_exchange_strong(previous, now)) {
            return;
        }
        AD_LOG_INFO("AirPlay 镜像中：视频累计 {} 帧，音频累计 {} 帧",
                    video_frames.load(), audio_frames.load());
    }
};

namespace {

AirplayReceiverImpl* impl_of(void* cls) {
    return static_cast<AirplayReceiverImpl*>(cls);
}

// ---- 下面这一组是交给协议层的回调 -----------------------------------------
//
// 协议层调用它们时一个也不判空（已逐处确认），所以每一个都必须有实体。
// 用不到的那些返回中性值，注释说明为什么不做事。

void cb_conn_init(void* cls) {
    (void) cls;   // 每来一条 TCP 连接就调一次，不是「有设备连上」，不记日志
}

void cb_conn_destroy(void* cls) {
    (void) cls;
}

void cb_conn_feedback(void* cls) {
    (void) cls;
}

void cb_conn_reset(void* cls, int reason) {
    AirplayReceiverImpl* impl = impl_of(cls);
    AD_LOG_INFO("AirPlay 连接已重置（原因码 {}）", reason);
    if (impl->mirroring.exchange(false)) {
        IAirplayListener* listener = impl->listener_snapshot();
        if (listener != nullptr) {
            listener->on_mirror_stopped();
        }
    }
    IAirplayListener* listener = impl->listener_snapshot();
    if (listener != nullptr) {
        listener->on_client_disconnected();
    }
}

void cb_video_reset(void* cls, reset_type_t reset_type) {
    AirplayReceiverImpl* impl = impl_of(cls);
    AD_LOG_DEBUG("AirPlay 视频流重置（类型 {}）", static_cast<int>(reset_type));
    if (impl->mirroring.exchange(false)) {
        IAirplayListener* listener = impl->listener_snapshot();
        if (listener != nullptr) {
            listener->on_mirror_stopped();
        }
    }
}

void cb_video_process(void* cls, raop_ntp_t* ntp, video_decode_struct* data) {
    (void) ntp;
    AirplayReceiverImpl* impl = impl_of(cls);
    if (data == nullptr || data->data == nullptr || data->data_len <= 0) {
        return;
    }
    impl->note_video_frame(data->data_len, data->is_h265);

    IAirplayListener* listener = impl->listener_snapshot();
    if (listener != nullptr) {
        listener->on_video_frame(data->data, data->data_len, data->is_h265);
    }
}

void cb_audio_process(void* cls, raop_ntp_t* ntp, audio_decode_struct* data) {
    (void) ntp;
    AirplayReceiverImpl* impl = impl_of(cls);
    if (data == nullptr || data->data == nullptr || data->data_len <= 0) {
        return;
    }
    impl->audio_frames.fetch_add(1);

    IAirplayListener* listener = impl->listener_snapshot();
    if (listener != nullptr) {
        listener->on_audio_frame(data->data, data->data_len, static_cast<int>(data->ct));
    }
}

void cb_video_pause(void* cls) {
    (void) cls;
}

void cb_video_resume(void* cls) {
    (void) cls;
}

void cb_video_flush(void* cls) {
    (void) cls;
}

void cb_audio_flush(void* cls) {
    (void) cls;
}

void cb_video_report_size(void* cls, float* width_source, float* height_source,
                          float* width, float* height) {
    (void) cls;
    // width_source / height_source 是发送端报来的画面尺寸（输入）；
    // width / height 是「渲染目标尺寸」，由这里决定（输出）。
    // 空指针不是防多余的：源尺寸必须非空才能读，否则这里就是个空指针解引用，
    // 而它跑在镜像流的第一帧上 —— 崩在那里，用户看到的是「一点投屏就闪退」。
    if (width_source == nullptr || height_source == nullptr) {
        return;
    }

    AD_LOG_INFO("AirPlay 镜像画面尺寸 {}x{}", static_cast<int>(*width_source),
                static_cast<int>(*height_source));

    // 目标尺寸先跟源一致，平台侧再按窗口缩放。
    if (width != nullptr) {
        *width = *width_source;
    }
    if (height != nullptr) {
        *height = *height_source;
    }
}

void cb_mirror_video_running(void* cls, bool is_running) {
    AirplayReceiverImpl* impl = impl_of(cls);
    IAirplayListener* listener = impl->listener_snapshot();
    if (is_running) {
        if (impl->mirroring.exchange(true)) {
            return;   // 已经在镜像了，不重复通知
        }
        AD_LOG_INFO("AirPlay 屏幕镜像已开始");
        if (listener != nullptr) {
            listener->on_mirror_started();
        }
    } else {
        if (!impl->mirroring.exchange(false)) {
            return;
        }
        AD_LOG_INFO("AirPlay 屏幕镜像已结束");
        if (listener != nullptr) {
            listener->on_mirror_stopped();
        }
    }
}

// 发送端请求接入。
//
// 文档 6.2 的「连接确认弹窗 / PIN」最终要挂在这里 —— 目前一律放行，
// 与 DLNA 那条路径的行为保持一致（见 docs/发布说明.md 里「还不能做什么」）。
void cb_report_client_request(void* cls, char* deviceid, char* model, char* name,
                              bool* admit) {
    AirplayReceiverImpl* impl = impl_of(cls);
    const std::string device = deviceid != nullptr ? deviceid : "";
    const std::string model_text = model != nullptr ? model : "";
    const std::string name_text = name != nullptr ? name : "";

    AD_LOG_INFO("iPhone 请求连接：{}（{}，设备 {}）",
                name_text.empty() ? "未命名" : name_text,
                model_text.empty() ? "型号未知" : model_text,
                device.empty() ? "未知" : device);

    if (admit != nullptr) {
        *admit = true;
    }

    IAirplayListener* listener = impl->listener_snapshot();
    if (listener != nullptr) {
        listener->on_client_connected(device, model_text, name_text);
    }
}

void cb_display_pin(void* cls, char* pin) {
    (void) cls;
    // 我们不启用协议自带的密码/PIN（TXT 里 pw=false），这条不会被触达。
    // 真被触达了要看得见，否则就是「iOS 在等一个永远不出现的 PIN」。
    AD_LOG_WARN("AirPlay 要求显示配对 PIN：{}（当前实现不启用该流程）",
                pin != nullptr ? pin : "");
}

void cb_register_client(void* cls, const char* device_id, const char* pk_str,
                        const char* name) {
    (void) cls;
    (void) pk_str;
    AD_LOG_INFO("AirPlay 已配对设备：{}（{}）",
                name != nullptr ? name : "", device_id != nullptr ? device_id : "");
}

bool cb_check_register(void* cls, const char* pk_str) {
    (void) cls;
    (void) pk_str;
    // 一律当作「未配对」：配对状态由协议层自己的密钥文件维护，
    // 我们不做第二份白名单，否则重启后两边会不一致。
    return false;
}

const char* cb_passwd(void* cls, int* len) {
    (void) cls;
    if (len != nullptr) {
        *len = 0;
    }
    // 空表示不要求密码（TXT 里的 pw=false）。
    return nullptr;
}

void cb_export_dacp(void* cls, const char* active_remote, const char* dacp_id) {
    (void) cls;
    (void) active_remote;
    (void) dacp_id;
}

void cb_audio_set_metadata(void* cls, const void* buffer, int buflen) {
    (void) cls;
    (void) buffer;
    (void) buflen;
}

void cb_audio_set_coverart(void* cls, const void* buffer, int buflen) {
    (void) cls;
    (void) buffer;
    (void) buflen;
}

void cb_audio_stop_coverart_rendering(void* cls) {
    (void) cls;
}

void cb_audio_remote_control_id(void* cls, const char* dacp_id,
                                const char* active_remote_header) {
    (void) cls;
    (void) dacp_id;
    (void) active_remote_header;
}

void cb_audio_set_progress(void* cls, uint32_t* start, uint32_t* curr, uint32_t* end) {
    (void) cls;
    (void) start;
    (void) curr;
    (void) end;
}

void cb_audio_get_format(void* cls, unsigned char* ct, unsigned short* spf,
                         bool* usingScreen, bool* isMedia, uint64_t* audioFormat) {
    (void) cls;
    // 这是**输入**回调：协议层把与发送端已经谈好的音频格式交给我们，由渲染
    // 端据此选解码器。所以这里只读不写 —— 往这些指针里写会改掉谈好的协商
    // 结果，声音会变成噪声或者干脆没有，而且从代码上完全看不出来。
    AD_LOG_INFO("AirPlay 音频格式：压缩类型 {}，每帧 {} 采样，镜像伴音 {}，媒体 {}，格式码 {:#x}",
                ct != nullptr ? static_cast<int>(*ct) : -1,
                spf != nullptr ? static_cast<int>(*spf) : -1,
                usingScreen != nullptr ? (*usingScreen ? "是" : "否") : "未知",
                isMedia != nullptr ? (*isMedia ? "是" : "否") : "未知",
                audioFormat != nullptr ? *audioFormat : 0ULL);
}

double cb_audio_set_client_volume(void* cls) {
    AirplayReceiverImpl* impl = impl_of(cls);
    return static_cast<double>(impl->volume.load()) / 100.0;
}

void cb_audio_set_volume(void* cls, float volume) {
    AirplayReceiverImpl* impl = impl_of(cls);
    int percent = static_cast<int>(volume * 100.0f + 0.5f);
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    impl->volume.store(percent);

    IAirplayListener* listener = impl->listener_snapshot();
    if (listener != nullptr) {
        listener->on_volume_changed(percent);
    }
}

int cb_video_set_codec(void* cls, video_codec_t codec) {
    (void) cls;
    // 返回 0 表示接受这个编码。两种编码我们都只是把码流转交给渲染层，
    // 能不能解由平台解码器决定 —— 在这里替它拒绝掉，只会让 iOS
    // 回退到更低的质量，而不是给出更好的结果。
    AD_LOG_DEBUG("AirPlay 镜像视频编码：{}",
                 codec == VIDEO_CODEC_H265 ? "H.265" : "H.264");
    return 0;
}

void cb_on_video_play(void* cls, const char* location, const float start_position) {
    AirplayReceiverImpl* impl = impl_of(cls);
    const std::string url = location != nullptr ? location : "";
    AD_LOG_INFO("AirPlay 视频推送：{}（起播位置 {:.1f}s）", url,
                static_cast<double>(start_position));

    IAirplayListener* listener = impl->listener_snapshot();
    if (listener != nullptr) {
        listener->on_video_play(url, start_position);
    }
}

void cb_on_video_scrub(void* cls, const float position) {
    (void) cls;
    AD_LOG_DEBUG("AirPlay 视频跳转：{:.1f}s", static_cast<double>(position));
}

void cb_on_video_rate(void* cls, const float rate) {
    (void) cls;
    (void) rate;
}

void cb_on_video_stop(void* cls) {
    AirplayReceiverImpl* impl = impl_of(cls);
    AD_LOG_INFO("AirPlay 视频推送已停止");

    IAirplayListener* listener = impl->listener_snapshot();
    if (listener != nullptr) {
        listener->on_video_stop();
    }
}

void cb_on_video_acquire_playback_info(void* cls, playback_info_t* playback_video) {
    (void) cls;
    if (playback_video == nullptr) {
        return;
    }
    // 视频播放由平台播放器负责，位置与缓冲状态都在它那里，协议层问不到。
    // 给一组「正在正常播放、无缓冲压力」的中性值：如果谎报成卡顿，
    // 发送端会主动降码率，那是自己给自己找麻烦。
    std::memset(playback_video, 0, sizeof(playback_info_t));
    playback_video->rate = 1.0f;
    playback_video->ready_to_play = true;
    playback_video->playback_likely_to_keep_up = true;
}

float cb_on_video_playlist_remove(void* cls) {
    (void) cls;
    return 0.0f;
}

// UxPlay 的内部日志转进我们自己的日志系统。
//
// 这一步不是可选的：AirPlay 的失败几乎都以「iPhone 那边转圈然后放弃」的形式
// 出现，接收端这边不留痕迹的话，排查就只剩猜。而它的日志里恰好有配对、
// FairPlay 握手、流协商每一步的结果。
void cb_log(void* cls, int level, const char* msg) {
    (void) cls;
    if (msg == nullptr) {
        return;
    }
    switch (level) {
        case LOGGER_EMERG:
        case LOGGER_ALERT:
        case LOGGER_CRIT:
        case LOGGER_ERR:
            AD_LOG_ERROR("[AirPlay] {}", msg);
            break;
        case LOGGER_WARNING:
            AD_LOG_WARN("[AirPlay] {}", msg);
            break;
        default:
            AD_LOG_DEBUG("[AirPlay] {}", msg);
            break;
    }
}

}  // namespace

#else

// 不带协议层时给一个空壳。
//
// 留空壳而不是把整个类都用 #if 包起来，是为了让「有没有 AirPlay」只影响
// 这一个文件里的实现，不影响任何调用方的代码形状。
struct AirplayReceiverImpl {
    std::mutex mutex;
    IAirplayListener* listener = nullptr;
};

#endif  // ADISPLAY_HAVE_AIRPLAY_RECEIVER

// ===========================================================================

AirplayReceiver::AirplayReceiver() : impl_(new AirplayReceiverImpl()) {}

AirplayReceiver::~AirplayReceiver() {
    stop();
}

bool AirplayReceiver::start(const AirplayConfig& config, std::string* out_error) {
#if !ADISPLAY_HAVE_AIRPLAY_RECEIVER
    (void) config;
    if (out_error != nullptr) {
        *out_error = "这份构建没有编入 AirPlay 协议层";
    }
    return false;
#else
    if (impl_->running) {
        return true;
    }

    unsigned char hw_addr[6] = {0};
    if (!parse_mac_address(config.device_id, hw_addr, 6)) {
        if (out_error != nullptr) {
            *out_error = "设备 ID 不是合法的 MAC 地址：" + config.device_id;
        }
        return false;
    }

    // 密钥文件必须真的能写：写不进去的话协议层每次启动都会重新生成密钥，
    // 表现是 iPhone 每次投屏都要重新配对。这种失败要在这里就说清楚，
    // 而不是等用户去猜为什么总要重配。
    if (!config.key_file.empty()) {
        std::error_code error_code;
        const std::filesystem::path key_path(config.key_file);
        std::filesystem::create_directories(key_path.parent_path(), error_code);
        // 目录建不出来不算致命：协议层会退化成内存密钥，功能仍然可用。
        if (error_code) {
            AD_LOG_WARN("AirPlay 密钥目录创建失败（{}），本次配对的密钥不会持久化：{}",
                        key_path.parent_path().string(), error_code.message());
        }
    }

    // 参数顺序看着别扭（error 在 pin_pw 之前），但那是上游这个版本的签名，
    // 照它写就行 —— 自作主张调换会得到一句「冲突的声明」，而不是参数错位的警告。
    int dnssd_error = 0;
    dnssd_t* dnssd = dnssd_init(config.device_name.c_str(),
                                static_cast<int>(config.device_name.size()),
                                reinterpret_cast<const char*>(hw_addr), 6,
                                &dnssd_error, 0);
    if (dnssd == nullptr) {
        if (out_error != nullptr) {
            *out_error = "初始化 AirPlay 设备身份失败（错误码 " +
                         std::to_string(dnssd_error) + "）";
        }
        return false;
    }

    raop_callbacks_t callbacks;
    std::memset(&callbacks, 0, sizeof(callbacks));
    callbacks.cls = impl_.get();
    callbacks.conn_init = &cb_conn_init;
    callbacks.conn_destroy = &cb_conn_destroy;
    callbacks.conn_feedback = &cb_conn_feedback;
    callbacks.conn_reset = &cb_conn_reset;
    callbacks.video_reset = &cb_video_reset;
    callbacks.video_process = &cb_video_process;
    callbacks.audio_process = &cb_audio_process;
    callbacks.video_pause = &cb_video_pause;
    callbacks.video_resume = &cb_video_resume;
    callbacks.video_flush = &cb_video_flush;
    callbacks.audio_flush = &cb_audio_flush;
    callbacks.video_report_size = &cb_video_report_size;
    callbacks.mirror_video_running = &cb_mirror_video_running;
    callbacks.report_client_request = &cb_report_client_request;
    callbacks.display_pin = &cb_display_pin;
    callbacks.register_client = &cb_register_client;
    callbacks.check_register = &cb_check_register;
    callbacks.passwd = &cb_passwd;
    callbacks.export_dacp = &cb_export_dacp;
    callbacks.audio_set_metadata = &cb_audio_set_metadata;
    callbacks.audio_set_coverart = &cb_audio_set_coverart;
    callbacks.audio_stop_coverart_rendering = &cb_audio_stop_coverart_rendering;
    callbacks.audio_remote_control_id = &cb_audio_remote_control_id;
    callbacks.audio_set_progress = &cb_audio_set_progress;
    callbacks.audio_get_format = &cb_audio_get_format;
    callbacks.audio_set_client_volume = &cb_audio_set_client_volume;
    callbacks.audio_set_volume = &cb_audio_set_volume;
    callbacks.video_set_codec = &cb_video_set_codec;
    callbacks.on_video_play = &cb_on_video_play;
    callbacks.on_video_scrub = &cb_on_video_scrub;
    callbacks.on_video_rate = &cb_on_video_rate;
    callbacks.on_video_stop = &cb_on_video_stop;
    callbacks.on_video_acquire_playback_info = &cb_on_video_acquire_playback_info;
    callbacks.on_video_playlist_remove = &cb_on_video_playlist_remove;

    raop_t* raop = raop_init(&callbacks);
    if (raop == nullptr) {
        dnssd_destroy(dnssd);
        if (out_error != nullptr) {
            *out_error = "初始化 AirPlay 协议层失败";
        }
        return false;
    }

    raop_set_log_callback(raop, &cb_log, nullptr);
    // DEBUG 级别。
    //
    // 协议层把「收到哪个请求、走了哪条分支」记在 DEBUG 上，INFO 只看得到错误。
    // 而 AirPlay 失败的典型样子恰恰是「iPhone 那边转圈然后放弃」—— 接收端这边
    // 一句错误都没有，因为请求可能压根没到、或者走到了某个静默返回的分支。
    // 排查「能搜到、连不上」时唯一能依靠的就是这一层。
    //
    // 逐帧的数据要 DEBUG_DATA（它还高一级），那个量级不能开。
    raop_set_log_level(raop, LOGGER_DEBUG);

    // nohold=1：允许新设备抢占已有连接。
    //
    // 取 1 而不是上游的默认 0，是因为接收端是一台公共设备（电视、客厅电脑）。
    // nohold=0 时，上一台设备留下的连接没断开，新设备会直接收到 409
    // 「服务器已连接另一台客户端」—— 而 iOS 的连接常常在用户以为已经退出之后
    // 仍然挂着，表现就是「过一会儿谁都投不上了」。
    //
    // 传 keyfile 让配对密钥持久化，否则 iPhone 每次都要重新配对。
    if (raop_init2(raop, 1, config.device_id.c_str(), config.key_file.c_str()) != 0) {
        raop_destroy(raop);
        dnssd_destroy(dnssd);
        if (out_error != nullptr) {
            *out_error = "AirPlay 协议层初始化失败（第二阶段）";
        }
        return false;
    }

    // 必须打开 HLS（AirPlay 视频）支持。
    //
    // 这一项不是可选的：协议层在收到「不带 CSeq 头」的请求时，会用这个开关
    // 判断它是不是来自 _airplay._tcp 的视频流 —— 关着的话那些请求会被
    // **静默忽略**（只在它自己的日志里留一句 use option -hls to activate
    // HLS support）。表现就是设备可见、点下去毫无反应。
    raop_set_plist(raop, "hls", 1);

    if (config.display_width > 0) {
        raop_set_plist(raop, "width", config.display_width);
    }
    if (config.display_height > 0) {
        raop_set_plist(raop, "height", config.display_height);
    }
    if (config.display_refresh_rate > 0) {
        raop_set_plist(raop, "refreshRate", config.display_refresh_rate);
    }
    if (config.display_max_fps > 0) {
        raop_set_plist(raop, "maxFPS", config.display_max_fps);
    }

    // 三个 UDP 口与两个 TCP 口都交给系统分配（0 表示自动），
    // 免得为了避开别的程序去维护一张固定端口表。
    unsigned short udp_ports[3] = {0, 0, 0};
    unsigned short tcp_ports[2] = {0, 0};
    raop_set_udp_ports(raop, udp_ports);
    raop_set_tcp_ports(raop, tcp_ports);

    unsigned short port = config.port;
    raop_start_httpd(raop, &port);
    raop_set_port(raop, port);
    if (port == 0) {
        raop_destroy(raop);
        dnssd_destroy(dnssd);
        if (out_error != nullptr) {
            *out_error = "AirPlay 控制通道端口绑定失败（" +
                         std::to_string(config.port) + "）";
        }
        return false;
    }

    // 把设备身份交给协议层：它会通过 dnssd_set_pk 把配对公钥塞进来，
    // 之后 /info 应答与我们的广播就都从同一处取值（见 DnssdShim.cpp）。
    raop_set_dnssd(raop, dnssd);

    std::string public_key;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->raop = raop;
        impl_->dnssd = dnssd;
        impl_->port = port;
        impl_->running = true;
        // 这两个值都从接缝里取，而不是从配置或 raop 的内部结构取：
        //   * 设备 id 必须用协议层规范化后的那份（小写）。/info 是拿同一段
        //     字节自己格式化出来的，广播跟着用同一个串才不会被 iOS 当成两台设备。
        //   * 公钥由协议层生成后交给接缝，我们只是转播。
        impl_->device_id_text = adisplay_dnssd_device_id(dnssd);
        impl_->public_key_text = adisplay_dnssd_public_key(dnssd);
        // 型号与版本也一并取走：广播要用它们，且必须与 /info 应答里的一致。
        impl_->model_text = adisplay_airplay_model();
        impl_->srcvers_text = adisplay_airplay_version();
        public_key = impl_->public_key_text;
    }

    if (public_key.empty()) {
        AD_LOG_WARN("AirPlay 未取得配对公钥，广播会缺 pk —— "
                    "iPhone 能看到本机但无法完成配对。请把日志发给开发者。");
    }
    return true;
#endif
}

void AirplayReceiver::stop() {
#if ADISPLAY_HAVE_AIRPLAY_RECEIVER
    raop_t* raop = nullptr;
    dnssd_t* dnssd = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running) {
            return;
        }
        raop = impl_->raop;
        dnssd = impl_->dnssd;
        impl_->raop = nullptr;
        impl_->dnssd = nullptr;
        impl_->running = false;
        impl_->port = 0;
    }

    // 先摘监听器：销毁过程中还会有回调在路上，让它们打到已经在拆的界面层
    // 上是崩溃的常见来源。
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->listener = nullptr;
    }
    impl_->mirroring.store(false);

    if (raop != nullptr) {
        raop_destroy(raop);
    }
    if (dnssd != nullptr) {
        dnssd_destroy(dnssd);
    }
#endif
}

bool AirplayReceiver::is_running() const {
#if ADISPLAY_HAVE_AIRPLAY_RECEIVER
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->running;
#else
    return false;
#endif
}

uint16_t AirplayReceiver::port() const {
#if ADISPLAY_HAVE_AIRPLAY_RECEIVER
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->port;
#else
    return 0;
#endif
}

std::string AirplayReceiver::device_id() const {
#if ADISPLAY_HAVE_AIRPLAY_RECEIVER
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->device_id_text;
#else
    return std::string();
#endif
}

std::string AirplayReceiver::public_key() const {
#if ADISPLAY_HAVE_AIRPLAY_RECEIVER
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->public_key_text;
#else
    return std::string();
#endif
}

std::string AirplayReceiver::model() const {
#if ADISPLAY_HAVE_AIRPLAY_RECEIVER
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->model_text;
#else
    return std::string();
#endif
}

std::string AirplayReceiver::srcvers() const {
#if ADISPLAY_HAVE_AIRPLAY_RECEIVER
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->srcvers_text;
#else
    return std::string();
#endif
}

void AirplayReceiver::set_listener(IAirplayListener* listener) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->listener = listener;
}

}  // namespace adisplay::airplay
