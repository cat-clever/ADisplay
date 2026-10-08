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

#include <adisplay/common/Config.h>
#include <adisplay/discovery/DiscoveryService.h>
#include <adisplay/dlna/DlnaRenderer.h>
#include <adisplay/common/DeviceIdentity.h>
#include <adisplay/common/DeviceName.h>
#include <adisplay/common/Log.h>
#include <adisplay/common/NetUtil.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
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
namespace dlna = adisplay::dlna;

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

bool is_valid_quality_preset(int preset) {
    return preset >= static_cast<int>(AD_QUALITY_SMOOTH) &&
           preset <= static_cast<int>(AD_QUALITY_SHARP);
}

}  // namespace

// ===========================================================================
// 引擎内部结构
// ===========================================================================

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
    if (!engine->persisted.has(kKeyDeviceId)) {
        if (engine->persist() != AD_OK) {
            AD_LOG_WARN("设备标识落盘失败，下次启动可能会变化");
        }
    }

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
        // 媒体管线要到批次 3 才接进来，现在没有 listener，
        // 所以控制动作会被记录但不会有画面。
        dlna_port = engine->dlna_renderer->port();
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
        discovery_config.device_id = engine->identity.device_id();
        discovery_config.uuid = engine->identity.uuid();
        discovery_config.airplay_port = airplay_port;
        discovery_config.dlna_port = dlna_port;
        discovery_config.castpc_port = castpc_port;
        discovery_config.enable_airplay = engine->enable_airplay;
        discovery_config.enable_dlna = engine->enable_dlna;
        discovery_config.enable_castpc = engine->enable_castpc;
        discovery_config.server_header = std::string("UPnP/1.0 ADisplay/") + ADISPLAY_VERSION;

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

    if (engine->dlna_renderer) {
        engine->dlna_renderer->stop();
        engine->dlna_renderer.reset();
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
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        engine->quality_preset = preset;
    }
    AD_LOG_INFO("画质档位已切换为 {}", preset);
    return engine->persist();
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
