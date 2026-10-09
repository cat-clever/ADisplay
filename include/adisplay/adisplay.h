/*
 * ADisplay —— 跨平台投屏接收端核心库（castcore）对外 C 接口
 * ---------------------------------------------------------------------------
 * 这是界面层与核心库之间【唯一】的契约。
 *
 *   Windows 界面 (C# + WinUI 3)  ->  P/Invoke
 *   macOS 界面   (Swift + SwiftUI) ->  bridging header
 *   Android TV   (Kotlin)          ->  JNI
 *
 * 设计约束（改动本文件前请先读）：
 *   1. 只使用 C99 类型。不出现 C++ 类型、STL 容器、异常、虚表跨边界。
 *   2. 所有字符串均为 UTF-8 且以 '\0' 结尾。
 *   3. 核心分配的内存由核心释放（ad_string_free / ad_buffer_free），
 *      调用方分配的内存由调用方释放。任何函数都不会替对方释放内存。
 *   4. 带 struct_size 字段的结构体用于 ABI 演进：调用方先填 struct_size，
 *      核心只读取它认得的部分。新增字段一律追加在末尾，不得插在中间。
 *   5. 回调可能在【任意线程】触发 —— 包括 ad_engine_start 这类同步接口的调用
 *      线程，以及核心内部的网络/解码工作线程。界面层必须自行 marshal 到 UI 线程，
 *      不要假设回调一定在后台线程。回调内不得调用 ad_engine_destroy / ad_engine_stop。
 *   6. 指针型回调参数（帧数据、字符串）只在【本次回调期间】有效，
 *      需要留存请自行拷贝。
 *
 * 许可：AGPL-3.0
 */

#ifndef ADISPLAY_ADISPLAY_H
#define ADISPLAY_ADISPLAY_H

#include <stddef.h>
#include <stdint.h>

/* ========================================================================
 * 导出宏
 * ===================================================================== */

#if defined(_WIN32) || defined(_WIN64)
#  if defined(ADISPLAY_CORE_BUILD)
#    define AD_API __declspec(dllexport)
#  else
#    define AD_API __declspec(dllimport)
#  endif
#  define AD_CALL __cdecl
#else
#  define AD_API __attribute__((visibility("default")))
#  define AD_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * 版本
 * ===================================================================== */

/* 接口版本。任何破坏性改动都必须递增，界面层据此拒绝加载不匹配的核心库。 */
#define AD_ABI_VERSION 1u

/* 返回形如 "0.1.0" 的库版本，静态存储，无需释放。 */
AD_API const char* AD_CALL ad_version_string(void);

/* 返回本头文件编译时的 ABI 版本。加载后先比对它和 AD_ABI_VERSION。 */
AD_API uint32_t AD_CALL ad_abi_version(void);

/* ========================================================================
 * 基础枚举
 * ===================================================================== */

/* 返回码。除 AD_OK 外均表示失败。 */
typedef enum AdResult {
    AD_OK                     = 0,
    AD_ERR_INVALID_ARG        = 1,  /* 参数非法（空指针、越界、名称不合规等） */
    AD_ERR_NOT_INITIALIZED    = 2,  /* 引擎尚未创建或已销毁 */
    AD_ERR_ALREADY_RUNNING    = 3,  /* 重复 start */
    AD_ERR_NOT_RUNNING        = 4,  /* 尚未 start 就调用了需要运行中的接口 */
    AD_ERR_PORT_IN_USE        = 5,  /* 端口被占用（macOS 上常见于系统自带 AirPlay 接收器占用 7000） */
    AD_ERR_PERMISSION_DENIED  = 6,  /* 缺少本地网络权限等 */
    AD_ERR_NETWORK            = 7,  /* 网络层错误 */
    AD_ERR_UNSUPPORTED        = 8,  /* 当前平台不支持该能力 */
    AD_ERR_BUFFER_TOO_SMALL   = 9,  /* 调用方给的缓冲区不够，out_len 已填入所需大小 */
    AD_ERR_NOT_FOUND          = 10, /* 找不到指定会话 / 请求 */
    AD_ERR_INTERNAL           = 11  /* 内部错误，详见日志 */
} AdResult;

/* 接收服务状态。通过 on_state_changed 上报。 */
typedef enum AdServiceState {
    AD_STATE_STOPPED      = 0,  /* 未启动 */
    AD_STATE_STARTING     = 1,  /* 正在注册 mDNS / 绑定端口 */
    AD_STATE_RUNNING      = 2,  /* 正在广播，等待手机连接 */
    AD_STATE_STREAMING    = 3,  /* 已有会话在投屏 */
    AD_STATE_STOPPING     = 4,  /* 正在注销服务 */
    AD_STATE_ERROR        = 5   /* 启动失败，详见 on_log 与 ad_engine_get_last_error */
} AdServiceState;

typedef enum AdLogLevel {
    AD_LOG_TRACE = 0,
    AD_LOG_DEBUG = 1,
    AD_LOG_INFO  = 2,
    AD_LOG_WARN  = 3,
    AD_LOG_ERROR = 4,
    AD_LOG_OFF   = 5
} AdLogLevel;

/* 画质档位（文档 2.3：低端电视盒子用「流畅」，桌面用「高清」）。
 *
 * 它落到实处的杠杆是**向发送端建议的显示尺寸**：手机把自己的屏幕缩放到这个
 * 尺寸再编码，投出来的视频就是这个分辨率。手机是竖屏，真正起作用的是高度 ——
 * 建议 1920x1080 时，手机量出来的是 498x1080，在 1080p 屏上 1:1 刚好，放到
 * Retina 全屏就明显发虚。所以档位是按高度往上抬的。
 *
 * 当前这一路镜像不受影响：尺寸在下次连接时才生效。 */
typedef enum AdQualityPreset {
    AD_QUALITY_SMOOTH   = 0,  /* 流畅：建议 1920x1080，代价最低 */
    AD_QUALITY_BALANCED = 1,  /* 均衡：建议 2560x1440 */
    AD_QUALITY_SHARP    = 2   /* 高清：建议 3840x2160，桌面全屏用这个 */
} AdQualityPreset;

/* 发送端类型。 */
typedef enum AdPeerKind {
    AD_PEER_UNKNOWN  = 0,
    AD_PEER_IOS      = 1,  /* AirPlay */
    AD_PEER_ANDROID  = 2,  /* DLNA / 自研协议 */
    AD_PEER_DESKTOP  = 3
} AdPeerKind;

/* 会话结束原因。 */
typedef enum AdCloseReason {
    AD_CLOSE_USER_REQUEST   = 0,  /* 发送端主动断开 / TEARDOWN */
    AD_CLOSE_NETWORK        = 1,  /* 网络断开 */
    AD_CLOSE_REJECTED       = 2,  /* 接收端拒绝 */
    AD_CLOSE_TIMEOUT        = 3,  /* 心跳超时 */
    AD_CLOSE_REPLACED       = 4,  /* 被新会话抢占（多设备排队策略） */
    AD_CLOSE_INTERNAL_ERROR = 5
} AdCloseReason;

/* 视频帧像素格式。解码后送渲染的数据格式。 */
typedef enum AdFrameFormat {
    AD_FORMAT_UNKNOWN    = 0,
    AD_FORMAT_NV12       = 1,  /* 硬解常见输出 */
    AD_FORMAT_I420       = 2,
    AD_FORMAT_BGRA8      = 3,  /* 软解 / 截图 */
    AD_FORMAT_P010       = 4   /* 10bit，HDR */
} AdFrameFormat;

/* 音视频流的类型，用于区分同一会话里的多条流。 */
typedef enum AdStreamKind {
    AD_STREAM_MIRROR_VIDEO = 0,
    AD_STREAM_MIRROR_AUDIO = 1,
    AD_STREAM_MEDIA_URL    = 2   /* DLNA / AirPlay 视频推送：只给 URL，由接收端自行拉流 */
} AdStreamKind;

/*
 * 播放状态。取值与 UPnP AVTransport 的 TransportState 一一对应 ——
 * 手机端拿它决定显示播放还是暂停按钮，含义必须精确对上。
 */
typedef enum AdTransportState {
    AD_TRANSPORT_NO_MEDIA_PRESENT = 0,  /* 还没有收到媒体 */
    AD_TRANSPORT_STOPPED          = 1,
    AD_TRANSPORT_PLAYING          = 2,
    AD_TRANSPORT_PAUSED           = 3,  /* 对应 UPnP 的 PAUSED_PLAYBACK */
    AD_TRANSPORT_TRANSITIONING    = 4   /* 正在起播 / 拖动中 */
} AdTransportState;

/*
 * 手机发来的播放控制意图。
 *
 * 核心自己不播放（DLNA 给的是 URL），所以这些意图会原样转给界面层，
 * 由界面层用平台播放器执行 —— 见 AdCallbacks.on_playback_command。
 */
typedef enum AdPlaybackCommand {
    AD_CMD_PLAY       = 0,
    AD_CMD_PAUSE      = 1,
    AD_CMD_STOP       = 2,
    AD_CMD_SEEK       = 3,  /* value 是目标位置（毫秒） */
    AD_CMD_SET_VOLUME = 4,  /* value 是 0..100 */
    AD_CMD_SET_MUTE   = 5   /* value 是 0 或 1 */
} AdPlaybackCommand;

/* ========================================================================
 * 结构体
 * ===================================================================== */

/*
 * 引擎配置。调用 ad_engine_get_default_config() 取得带默认值的实例后再改。
 * struct_size 必须填 sizeof(AdConfig)。
 */
typedef struct AdConfig {
    uint32_t         struct_size;      /* = sizeof(AdConfig) */
    uint32_t         abi_version;      /* = AD_ABI_VERSION */

    /* 设备名称（UTF-8，1–32 字符，见 ad_device_name_validate）。
       为 NULL 时核心自行取主机名（桌面端）或设备型号（电视端）。 */
    const char*      device_name;

    /* 监听端口。填 0 表示用默认值。 */
    uint16_t         airplay_port;     /* 默认 7000，被占用时自动 +1 试探 */
    uint16_t         dlna_port;        /* 默认 49152 */
    uint16_t         castpc_port;      /* 默认 8765，自研协议 */

    /* 是否在启动时自动开启各协议服务。 */
    int              enable_airplay;   /* 默认 1 */
    int              enable_dlna;      /* 默认 1 */
    int              enable_castpc;    /* 默认 1 */

    /* 画质档位，见 AdQualityPreset。 */
    int              quality_preset;   /* 默认 AD_QUALITY_BALANCED */

    /* 首次连接是否需要确认。1 = 弹窗确认（默认），0 = 直接放行。 */
    int              require_confirmation;

    /* 日志级别，见 AdLogLevel。 */
    int              log_level;        /* 默认 AD_LOG_INFO */

    /* 日志文件路径（UTF-8）。NULL 表示只写内存缓冲、不落盘。 */
    const char*      log_file_path;

    /* 配置文件的读写路径（UTF-8）。NULL 表示用平台默认位置。
       设备名称、白名单等持久化在这里（文档 2.4）。 */
    const char*      config_file_path;

    /* 只在这些网卡上广播。NULL 表示自动（忽略 VMware / WSL / VPN 等虚拟网卡，
       见文档 6.3）。数组元素是网卡名 UTF-8 字符串，如 "en0"、"以太网"。 */
    const char* const* bind_interfaces;
    uint32_t         bind_interface_count;
} AdConfig;

/*
 * 发送端信息。由核心填充，字符串指针在回调期间有效。
 */
typedef struct AdPeerInfo {
    uint32_t    struct_size;      /* = sizeof(AdPeerInfo) */
    uint32_t    reserved;

    const char* display_name;     /* 手机名称，如 "张三的 iPhone" */
    const char* address;          /* 点分十进制 / IPv6 字面量 */
    uint16_t    port;
    uint16_t    reserved2;
    int         kind;             /* AdPeerKind */
    const char* model;            /* 设备型号，可能为 NULL */
    const char* os_version;       /* 系统版本，可能为 NULL */
} AdPeerInfo;

/*
 * 一帧解码后的视频。
 *
 * data 指向核心持有的缓冲区，【仅在本次回调期间有效】。需要留存请自行拷贝。
 * plane_count 与 linesize 描述平面布局：NV12 是 2 个平面，I420 是 3 个，
 * BGRA8 是 1 个。linesize[i] 是第 i 个平面每行的字节数（可能大于 width）。
 */
/*
 * 一帧**压缩的**镜像视频（AirPlay 屏幕镜像）。
 *
 * 为什么与 AdVideoFrame 分开：那个是「核心解码、界面渲染」那条路用的（交出来
 * 的是 NV12/I420 这类平面数据）。镜像走另一条更短的路 —— 直接把压缩帧交给各平台
 * 自带的解码器（macOS 的 AVSampleBufferDisplayLayer、Windows 的 Media Foundation、
 * 电视端的 MediaCodec）。它们硬解现成、延迟最低，也省掉在核心里养一套解码加
 * 渲染的代码。
 *
 * data 是 **Annex B**：每个 NALU 前面是 00 00 01 或 00 00 00 01 起始码，内容已经
 * 解密。指向核心持有的缓冲区，【仅在本次回调期间有效】。
 *
 * 编码参数（H.264 的 SPS/PPS，H.265 的 VPS/SPS/PPS）以 NALU 的形式跟在帧里，
 * 而且**核心保证每个关键帧都带上**，不只是第一帧 —— 协议层原本只在流开头给一次，
 * 而界面层的渲染面往往晚一步才挂上来（帧回调在核心线程上立刻触发，会话回调却要
 * 排队回主线程才能建出视图），那样第一帧就被丢在还没有落点的时候，参数集此后再也
 * 不来，表现是「帧一直在计数、屏幕始终全黑」。
 *
 * 平台解码器多半要 AVCC（4 字节长度前缀），那一步转换归界面层 ——
 * 见 macOS 的 MirrorView.swift 与 Windows 的 MirrorStreamSource.cs。
 */
typedef struct AdMirrorFrame {
    uint32_t    struct_size;   /* = sizeof(AdMirrorFrame) */
    uint32_t    session_id;
    const uint8_t* data;
    int32_t     size;
    /* 0 = H.264，1 = H.265。 */
    int32_t     is_h265;
    /* 发送端报来的画面尺寸，可能为 0（还没收到尺寸信息）。 */
    uint32_t    width;
    uint32_t    height;
    /* 显示时间戳，微秒。用于定帧率；为 0 表示发送端没给。 */
    int64_t     pts_us;
    int         reserved;
} AdMirrorFrame;

/*
 * 一帧**压缩的**镜像伴音（AirPlay 屏幕镜像的伴音）。
 *
 * 与 AdMirrorFrame 同一个思路：能交给平台解的就不在核心解。核心按「界面层注册
 * 了哪一个回调」决定这一帧怎么走：
 *
 *   注册了本回调       → 原样转发压缩帧，核心不做解码
 *   只注册 on_audio_frame → 核心解成 PCM 再发
 *
 * Android 走前者：它的 MediaCodec 认 AAC-ELD，而把 FFmpeg 静态链进 APK 会让包
 * 大出上百兆（见 core/pipeline/CMakeLists.txt 里的实测）。macOS / Windows 走
 * 后者：AudioToolbox 认，但 Media Foundation 不认，与其分平台写两套不如核心解。
 *
 * data 是 AAC-ELD 的裸帧（不含 ADTS 头），内容已经解密，仅在本次回调期间有效。
 */
typedef struct AdMirrorAudioFrame {
    uint32_t    struct_size;   /* = sizeof(AdMirrorAudioFrame) */
    uint32_t    session_id;
    const uint8_t* data;
    int32_t     size;
    /* 采样率与声道数。每帧长度由编码配置决定，这里一并给出，省得界面层自己辨认。 */
    uint32_t    sample_rate;
    uint32_t    channels;
    int64_t     pts_us;
    int         reserved;
} AdMirrorAudioFrame;

typedef struct AdVideoFrame {
    uint32_t    struct_size;      /* = sizeof(AdVideoFrame) */
    uint32_t    session_id;
    int64_t     pts_us;           /* 显示时间戳，微秒，用于音画同步 */
    uint32_t    width;
    uint32_t    height;
    int         format;           /* AdFrameFormat */
    int         plane_count;
    const uint8_t* data[4];       /* 各平面起始地址 */
    int32_t     linesize[4];      /* 各平面行跨度，单位字节 */

    /* 旋转角度，0 / 90 / 180 / 270。手机横竖屏切换时变化（文档 2.1）。 */
    int         rotation_degrees;
    int         reserved;
} AdVideoFrame;

/*
 * 一帧解码后的音频。样本为交错布局（LRLRLR...），32 位浮点。
 * data 同样只在本次回调期间有效。
 */
typedef struct AdAudioFrame {
    uint32_t    struct_size;      /* = sizeof(AdAudioFrame) */
    uint32_t    session_id;
    int64_t     pts_us;
    uint32_t    sample_rate;
    uint32_t    channels;
    uint32_t    frame_count;      /* 每声道样本数 */
    uint32_t    reserved;
    const float* data;
} AdAudioFrame;

/*
 * 事件回调集合。
 *
 * struct_size 必须填 sizeof(AdCallbacks)。任何一项都可以为 NULL，核心会跳过。
 * 回调可能在任意线程触发（含调用 ad_engine_start 的线程）—— 界面层必须自行
 * 切回 UI 线程，不要假设它一定在后台线程。
 */
typedef struct AdCallbacks {
    uint32_t    struct_size;      /* = sizeof(AdCallbacks) */
    uint32_t    reserved;

    /* 服务状态变化。state 见 AdServiceState。 */
    void (AD_CALL *on_state_changed)(void* user_data, int state);

    /* 有设备请求连接，需要界面弹窗确认（文档 2.1 第 5 条）。
       界面层拿到 request_id 后调用 ad_engine_respond_connect_request()。
       若 AdConfig.require_confirmation 为 0，此项不会被触发。 */
    void (AD_CALL *on_connect_request)(void* user_data, const AdPeerInfo* peer, uint32_t request_id);

    /* 会话建立 / 结束。 */
    void (AD_CALL *on_session_opened)(void* user_data, uint32_t session_id, const AdPeerInfo* peer, int stream_kind);
    void (AD_CALL *on_session_closed)(void* user_data, uint32_t session_id, int reason);

    /* 解码后的音视频帧。stream_kind 见 AdStreamKind。 */
    void (AD_CALL *on_video_frame)(void* user_data, const AdVideoFrame* frame);
    void (AD_CALL *on_audio_frame)(void* user_data, const AdAudioFrame* frame);

    /* 镜像流的压缩视频帧（AirPlay 屏幕镜像专用，见 AdMirrorFrame）。
       界面层把它交给平台解码器即可，核心不做解码。 */
    void (AD_CALL *on_mirror_frame)(void* user_data, const AdMirrorFrame* frame);

    /* 收到媒体 URL（DLNA / AirPlay 视频推送，文档 3.2）。
       接收端据此自行拉流播放，或交给 libmpv。 */
    void (AD_CALL *on_media_url)(void* user_data, uint32_t session_id, const char* url, const char* mime_type);

    /* 播放控制状态变化，供界面同步进度条 / 音量（DLNA 事件回推）。 */
    void (AD_CALL *on_playback_state)(void* user_data, uint32_t session_id, int transport_state,
                                      int64_t position_ms, int64_t duration_ms, int volume);

    /* 手机发来的播放控制意图：播放 / 暂停 / 停止 / 跳转 / 调音量 / 静音。
       command 见 AdPlaybackCommand，value 的含义见该枚举的注释。

       界面层拿自己的播放器执行，然后把结果用 ad_engine_report_playback()
       回报给核心 —— 手机端的进度条和音量滑块靠那份回报更新。 */
    void (AD_CALL *on_playback_command)(void* user_data, uint32_t session_id, int command, int64_t value);

    /* 日志。level 见 AdLogLevel。msg 为 UTF-8，只在回调期间有效。 */
    void (AD_CALL *on_log)(void* user_data, int level, const char* msg);

    /* 镜像伴音的**压缩**帧（见 AdMirrorAudioFrame）。
       注册它就等于告诉核心「这一帧我自己解」—— 核心只做转发，不做解码。 */
    void (AD_CALL *on_mirror_audio_frame)(void* user_data, const AdMirrorAudioFrame* frame);
} AdCallbacks;

/* ========================================================================
 * 引擎生命周期
 * ===================================================================== */

/* 不透明句柄。界面层不关心它的内部结构。 */
typedef struct AdEngine AdEngine;

/*
 * 取一份带默认值的配置。传入的指针不得为 NULL。
 * 用法：AdConfig cfg; ad_engine_get_default_config(&cfg); cfg.device_name = "客厅电脑";
 * 返回 AD_OK 或 AD_ERR_INVALID_ARG。
 */
AD_API AdResult AD_CALL ad_engine_get_default_config(AdConfig* out_config);

/*
 * 创建引擎。config 为 NULL 时使用全部默认值。
 * 成功后 *out_engine 持有句柄，用完必须 ad_engine_destroy()。
 * 注意：创建不等于启动，端口绑定和 mDNS 注册发生在 ad_engine_start()。
 */
AD_API AdResult AD_CALL ad_engine_create(const AdConfig* config, AdEngine** out_engine);

/*
 * 销毁引擎。会先隐式 stop，并等待工作线程退出。
 * 传入 NULL 是安全的空操作。
 */
AD_API void AD_CALL ad_engine_destroy(AdEngine* engine);

/*
 * 启动接收服务：绑定端口、注册 mDNS 与 SSDP 广播。
 *
 * 失败时返回具体原因 —— AD_ERR_PORT_IN_USE 需要界面层提示用户
 * （macOS 12+ 系统自带 AirPlay 接收器会占用 7000，见文档 4.5）。
 */
AD_API AdResult AD_CALL ad_engine_start(AdEngine* engine);

/* 停止接收服务并断开所有会话。未启动时调用是安全的空操作。 */
AD_API void AD_CALL ad_engine_stop(AdEngine* engine);

/* 查询当前状态，见 AdServiceState。 */
AD_API AdResult AD_CALL ad_engine_get_state(AdEngine* engine, int* out_state);

/*
 * 注册回调。必须在 ad_engine_start() 之前调用。
 * user_data 会原样回传给每个回调，由调用方管理生命周期。
 * 传 NULL 的 callbacks 表示清空所有回调。
 */
AD_API AdResult AD_CALL ad_engine_set_callbacks(AdEngine* engine,
                                                const AdCallbacks* callbacks,
                                                void* user_data);

/* 取最近一次失败的详细描述（UTF-8）。buf 为空或太小返回 AD_ERR_BUFFER_TOO_SMALL
   并填入所需大小。返回的字符串写入调用方的缓冲区，不需要释放。 */
AD_API AdResult AD_CALL ad_engine_get_last_error(AdEngine* engine,
                                                 char* buf, size_t buf_size, size_t* out_len);

/* ========================================================================
 * 配置与设备名称（文档 2.4）
 * ===================================================================== */

/*
 * 校验设备名称是否合规。规则：1–32 个字符，不允许控制字符，不能为空。
 * 返回 AD_OK 表示合规。
 * 不合规时，若 reason_buf 非空，会写入一句人话说明（UTF-8）；界面层可直接显示。
 */
AD_API AdResult AD_CALL ad_device_name_validate(const char* utf8_name,
                                                char* reason_buf, size_t reason_size);

/*
 * 修改设备名称并【即时生效】：核心会注销并重新注册 mDNS（AirPlay、自研协议）
 * 与 SSDP 显示名（DLNA），无需重启软件。设备唯一标识与已配对记录保持不变，
 * 已连接过的手机不需要重新配对（文档 2.4）。
 * 名称不合规返回 AD_ERR_INVALID_ARG。
 */
AD_API AdResult AD_CALL ad_engine_set_device_name(AdEngine* engine, const char* utf8_name);

/* 读取当前设备名称，写入调用方缓冲区。不需要时打印到 UI 上。 */
AD_API AdResult AD_CALL ad_engine_get_device_name(AdEngine* engine,
                                                  char* buf, size_t buf_size, size_t* out_len);

/* 切换画质档位，见 AdQualityPreset。 */
AD_API AdResult AD_CALL ad_engine_set_quality_preset(AdEngine* engine, int preset);
/* AirPlay 广播所需的一切（文档 3.1），供「自己发不了 mDNS 的平台」使用。
 *
 * 为什么要有它：Android 上发布 mDNS 只能用 Java 层的 NsdManager，核心够不着
 * （见 platform/android/src/MdnsPublisherAndroid.cpp），所以广播内容必须跨过
 * ABI 交给界面层，由它去注册。桌面上这件事由核心自己做，不需要这个接口。
 *
 * 输出是若干行 key=value（UTF-8），格式见 AirplayAdvert.h 的 advert_for_platform：
 *
 *     port=7001
 *     airplay_name=<显示名>
 *     raop_name=<deviceid>@<显示名>
 *     airplay.<TXT 键>=<值的十六进制>
 *     raop.<TXT 键>=<值的十六进制>
 *
 * 必须在服务启动之后调用 —— 端口那时才确定。缓冲区不够时返回 AD_ERR_BUFFER_TOO_SMALL。 */
AD_API AdResult AD_CALL ad_engine_get_airplay_advert(AdEngine* engine, char* buffer,
                                                    size_t buffer_size, size_t* out_length);
/* 读取当前档位（取值见 AdQualityPreset）。界面启动时用它把选择器拨到已保存的那一档。 */
AD_API AdResult AD_CALL ad_engine_get_quality_preset(AdEngine* engine, int* out_preset);

/* 把当前配置写回配置文件（设备名称、端口、白名单等）。 */
AD_API AdResult AD_CALL ad_engine_save_config(AdEngine* engine);

/* ========================================================================
 * 连接确认与会话管理
 * ===================================================================== */

/*
 * 回应 on_connect_request。allow 非 0 表示允许，remember 非 0 表示加入白名单
 * （对应界面的「始终允许」，文档 7 第 3 条）。
 * request_id 来自回调，超时未回应会被核心视为拒绝。
 */
AD_API AdResult AD_CALL ad_engine_respond_connect_request(AdEngine* engine,
                                                          uint32_t request_id,
                                                          int allow, int remember);

/* 主动断开某个会话。 */
AD_API AdResult AD_CALL ad_engine_disconnect_session(AdEngine* engine, uint32_t session_id);

/*
 * 界面层播放器的当前状态。
 *
 * 界面层是播放状态的唯一权威来源：核心不碰播放器，手机的 GetTransportInfo /
 * GetPositionInfo / GetMediaInfo / GetVolume 全部从这里取答案。所以界面层在
 * 状态真的变了的时候必须回报一次，否则手机会一直看到旧值。
 *
 * position_ms / duration_ms 填 -1 表示「还不知道」；volume 与 muted 填 -1 表示
 * 「这项没变」—— 用 -1 而不是 0，是因为 0 是合法值（音量 0、未静音），
 * 拿 0 当「不变」会把真实状态冲掉。
 */
typedef struct AdPlaybackStatus {
    uint32_t    struct_size;      /* = sizeof(AdPlaybackStatus) */
    uint32_t    abi_version;      /* = AD_ABI_VERSION */

    uint32_t    session_id;       /* 来自 on_media_url / on_session_opened */
    uint32_t    reserved;

    int         transport_state;  /* AdTransportState */
    int64_t     position_ms;      /* 当前播放位置，未知填 -1 */
    int64_t     duration_ms;      /* 总时长，未知填 -1 */
    int         volume;           /* 0..100，-1 表示不变 */
    int         muted;            /* 0 或 1，-1 表示不变 */
} AdPlaybackStatus;

/*
 * 回报播放器状态。状态确实变化时核心会把对应的事件推给手机（GENA）。
 * 未运行或 session_id 对不上时返回 AD_ERR_NOT_FOUND。
 */
AD_API AdResult AD_CALL ad_engine_report_playback(AdEngine* engine, const AdPlaybackStatus* status);

/* 取当前会话数。多设备排队策略见文档 4.2 SessionManager。 */
AD_API AdResult AD_CALL ad_engine_get_session_count(AdEngine* engine, uint32_t* out_count);

/* 取当前会话列表。peers 由调用方分配，容量为 capacity。
   out_count 回填实际数量。 */
AD_API AdResult AD_CALL ad_engine_get_sessions(AdEngine* engine,
                                               AdPeerInfo* peers, uint32_t capacity,
                                               uint32_t* out_count);

/* ========================================================================
 * 本地信息（供待机页显示，文档 2.3）
 * ===================================================================== */

/*
 * 取本机在局域网中的地址列表，换行分隔的 UTF-8 字符串，形如：
 *   "192.168.1.20\nfe80::1c2d:3e4f:5a6b:7c8d"
 * 电视端待机页用它显示连接说明。
 */
AD_API AdResult AD_CALL ad_engine_get_local_addresses(AdEngine* engine,
                                                      char* buf, size_t buf_size, size_t* out_len);

/* 取设备唯一标识（deviceid / UUID）。改名不影响它（文档 2.4「标识不变」）。 */
AD_API AdResult AD_CALL ad_engine_get_device_id(AdEngine* engine,
                                                char* buf, size_t buf_size, size_t* out_len);

/* ========================================================================
 * 工具
 * ===================================================================== */

/* 返回码对应的英文简述，静态存储。用于日志，不要直接显示给用户。 */
AD_API const char* AD_CALL ad_result_string(AdResult result);

/* 释放核心分配的字符串。传 NULL 是安全的空操作。 */
AD_API void AD_CALL ad_string_free(char* str);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* ADISPLAY_ADISPLAY_H */
