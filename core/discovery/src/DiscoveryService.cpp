// ADisplay —— 服务发现统一管理实现
#include <adisplay/discovery/DiscoveryService.h>

#include <adisplay/common/DeviceName.h>
#include <adisplay/common/Log.h>
#include <adisplay/common/NetUtil.h>

#include <adisplay/discovery/MdnsPublisher.h>
#include <adisplay/discovery/SsdpServer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace adisplay::discovery {
namespace {

// 网络地址的轮询间隔。文档 6.3 要求「监听网络变化事件，重新注册服务」——
// 各平台的事件接口差异很大（macOS 是 SCDynamicStore、Windows 是
// NotifyIpInterfaceChange、Linux 是 netlink），这里先用轮询实现同样的效果。
// 3 秒对「切换 Wi-Fi 后几秒内恢复」这个体验目标是够的，且行为跨平台一致。
constexpr int kNetworkPollSeconds = 3;

// AirPlay 的能力位（features）。
//
// 这个值决定 iPhone 是否在控制中心里显示「屏幕镜像」入口 —— 位不对的话
// 设备能出现在列表里但点不动，是 AirPlay 对接最容易卡住的地方。
// 这里用的是公开实现普遍采用的一组值，批次 4 接镜像流时会按实际协议行为校准。
constexpr const char* kAirplayFeatures = "0x5A7FFFF7,0x1E";

// 期望在局域网里被看到的服务类型。
constexpr const char* kAirplayServiceType = "_airplay._tcp";
constexpr const char* kRaopServiceType = "_raop._tcp";
constexpr const char* kCastpcServiceType = "_castpc._tcp";

// DLNA 的 MediaRenderer 设备类型与三个必需服务，见文档 3.2。
constexpr const char* kMediaRendererType = "urn:schemas-upnp-org:device:MediaRenderer:1";
constexpr const char* kAvTransportType = "urn:schemas-upnp-org:service:AVTransport:1";
constexpr const char* kRenderingControlType = "urn:schemas-upnp-org:service:RenderingControl:1";
constexpr const char* kConnectionManagerType = "urn:schemas-upnp-org:service:ConnectionManager:1";

// _raop._tcp 的实例名有固定格式：<deviceid>@<显示名>。
// iPhone 靠这个字段把 RAOP 音频流关联回 _airplay._tcp 那条记录。
std::string raop_instance_name(const DiscoveryConfig& config) {
    std::string name = config.device_id;
    name += '@';
    name += config.device_name;
    return name;
}

std::vector<TxtRecord> build_airplay_txt(const DiscoveryConfig& config) {
    return {
        {"deviceid", config.device_id},
        {"features", kAirplayFeatures},
        {"model", config.airplay_model},
        {"srcvers", config.airplay_srcvers},
        {"pi", config.uuid},
        // flags 的 bit 2（0x4）表示「支持音频」。留 0 会让部分 iOS 版本
        // 认为这个接收端只能收视频。
        {"flags", "0x4"},
        {"vv", "2"},
    };
}

std::vector<TxtRecord> build_raop_txt(const DiscoveryConfig& config) {
    return {
        // ch=2 立体声；cn 是支持的音频编码（0=PCM, 1=ALAC, 2=AAC, 3=AAC-ELD）。
        {"ch", "2"},
        {"cn", "0,1,2,3"},
        {"et", "0,3,5"},
        {"ft", kAirplayFeatures},
        {"md", "0,1,2"},
        {"am", config.airplay_model},
        {"tp", "UDP"},
        {"vv", "2"},
        {"vs", config.airplay_srcvers},
        {"sr", "44100"},
        {"ss", "16"},
        // pw=false：不用密码。首连确认由我们自己的 PIN 流程负责（文档 6.2），
        // 不走 AirPlay 自带的密码机制。
        {"pw", "false"},
        {"vn", "65537"},
    };
}

std::vector<TxtRecord> build_castpc_txt(const DiscoveryConfig& config) {
    return {
        {"deviceid", config.device_id},
        {"uuid", config.uuid},
        {"name", config.device_name},
        {"port", std::to_string(config.castpc_port)},
    };
}

std::string build_location(const std::string& address, uint16_t port) {
    std::ostringstream out;
    out << "http://" << address << ":" << port << "/description.xml";
    return out.str();
}

}  // namespace

// ===========================================================================

struct DiscoveryService::Impl {
    mutable std::mutex mutex;
    DiscoveryConfig config;
    std::vector<std::unique_ptr<IMdnsPublisher>> publishers;
    SsdpServer ssdp;
    std::atomic<bool> running{false};
    std::thread network_monitor;

    // 用条件变量代替「睡够 3 秒再看停止标志」的轮询。
    // 后者会让关闭操作必须等满一个轮询周期 —— 而 stop() 是从界面线程调的，
    // 用户看到的就是「点关闭卡一下、鼠标转圈」。
    std::condition_variable monitor_wakeup;
    std::mutex monitor_mutex;
    std::string current_address;
    std::string last_error;

    // 按当前配置与地址，把三条 mDNS 服务全部公布一遍。
    bool publish_all(const std::string& address, std::string* out_error) {
        publishers.clear();
        std::string first_failure;

        const auto add_publisher = [this, &address, &first_failure](
                                       const std::string& service_type,
                                       const std::string& instance_name,
                                       uint16_t port,
                                       std::vector<TxtRecord> txt) {
            if (port == 0) {
                return;
            }
            std::unique_ptr<IMdnsPublisher> publisher = create_mdns_publisher();
            if (!publisher) {
                return;
            }
            MdnsServiceInfo info;
            info.service_type = service_type;
            info.instance_name = instance_name;
            info.port = port;
            info.txt = std::move(txt);
            info.host_name = address;   // 让 A 记录指向我们选定的网卡

            std::string error;
            if (!publisher->publish(info, &error)) {
                if (first_failure.empty()) {
                    first_failure = service_type + "：" + error;
                }
                AD_LOG_WARN("mDNS 公布失败 {}：{}", service_type, error);
                return;
            }
            publishers.push_back(std::move(publisher));
        };

        if (config.enable_airplay) {
            add_publisher(kAirplayServiceType, config.device_name,
                          config.airplay_port, build_airplay_txt(config));
            // _raop._tcp 与 _airplay._tcp 用同一个端口，但实例名格式不同。
            add_publisher(kRaopServiceType, raop_instance_name(config),
                          config.airplay_port, build_raop_txt(config));
        }
        if (config.enable_castpc) {
            add_publisher(kCastpcServiceType, config.device_name,
                          config.castpc_port, build_castpc_txt(config));
        }

        if (out_error != nullptr) {
            *out_error = first_failure;
        }
        return !publishers.empty() || (!config.enable_airplay && !config.enable_castpc);
    }

    void withdraw_all() {
        for (std::unique_ptr<IMdnsPublisher>& publisher : publishers) {
            if (publisher) {
                publisher->withdraw();
            }
        }
        publishers.clear();
    }

    // SSDP 的 LOCATION 里带的地址必须是真的能被手机访问到的地址。
    std::string resolve_address() {
        if (!config.preferred_address.empty()) {
            return config.preferred_address;
        }
        return common::primary_local_ipv4();
    }

    bool start_ssdp(const std::string& address, std::string* out_error) {
        if (!config.enable_dlna) {
            return true;
        }
        if (address.empty()) {
            if (out_error != nullptr) {
                *out_error = "没有可用的局域网地址，DLNA 无法公布";
            }
            return false;
        }

        SsdpAdvertisement advertisement;
        advertisement.udn = "uuid:" + config.uuid;
        advertisement.device_type = kMediaRendererType;
        advertisement.location = build_location(address, config.dlna_port);
        advertisement.service_types = {
            kAvTransportType, kRenderingControlType, kConnectionManagerType,
        };
        advertisement.server_header = config.server_header;

        std::string error;
        if (!ssdp.start(advertisement, &error)) {
            if (out_error != nullptr) {
                *out_error = error;
            }
            AD_LOG_ERROR("SSDP 启动失败：{}", error);
            return false;
        }
        return true;
    }

    // 网卡变化的轮询。地址变了才重注册 —— 否则网卡一抖动
    // （比如 macOS 上 utun 接口的起落）就会重注册，手机端列表会闪。
    void monitor_loop() {
        std::string last_seen;
        {
            std::lock_guard<std::mutex> lock(mutex);
            last_seen = current_address;
        }

        while (running.load(std::memory_order_relaxed)) {
            // 等一个轮询周期，或者被 stop() 立刻唤醒。
            {
                std::unique_lock<std::mutex> lock(monitor_mutex);
                const bool stopping = monitor_wakeup.wait_for(
                    lock, std::chrono::seconds(kNetworkPollSeconds),
                    [this]() { return !running.load(std::memory_order_relaxed); });
                if (stopping) {
                    return;
                }
            }

            std::string address;
            {
                std::lock_guard<std::mutex> lock(mutex);
                address = resolve_address();
            }

            if (address == last_seen) {
                continue;
            }

            AD_LOG_INFO("检测到局域网地址变化：{} → {}，重新注册广播",
                        last_seen.empty() ? "（无）" : last_seen,
                        address.empty() ? "（无）" : address);

            republish(address);
            last_seen = address;
        }
    }

    // 用新地址重新公布全部服务。
    void republish(const std::string& address) {
        std::lock_guard<std::mutex> lock(mutex);
        current_address = address;

        withdraw_all();
        if (config.enable_dlna) {
            ssdp.stop();
        }

        std::string error;
        publish_all(address, &error);
        if (config.enable_dlna) {
            start_ssdp(address, &error);
        }
        if (!error.empty()) {
            last_error = error;
        }
    }

    void set_error(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        last_error = message;
    }
};

// ===========================================================================

DiscoveryService::DiscoveryService() : impl_(new Impl()) {}

DiscoveryService::~DiscoveryService() {
    stop();
}

bool DiscoveryService::start(const DiscoveryConfig& config, std::string* out_error) {
    if (impl_->running.load()) {
        return set_device_name(config.device_name, out_error);
    }

    const std::string address = config.preferred_address.empty()
                                    ? common::primary_local_ipv4()
                                    : config.preferred_address;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->config = config;
        impl_->current_address = address;
        impl_->last_error.clear();
    }

    if (address.empty()) {
        const std::string message =
            "没有检测到可用的局域网地址。请确认已连接 Wi-Fi 或网线。";
        impl_->set_error(message);
        if (out_error != nullptr) {
            *out_error = message;
        }
        AD_LOG_ERROR("{}", message);
        return false;
    }

    impl_->running.store(true);

    std::string mdns_error;
    const bool mdns_ok = impl_->publish_all(address, &mdns_error);

    std::string ssdp_error;
    const bool ssdp_ok = impl_->start_ssdp(address, &ssdp_error);

    if (!mdns_ok && !ssdp_ok) {
        impl_->running.store(false);
        const std::string message = !mdns_error.empty() ? mdns_error : ssdp_error;
        impl_->set_error(message);
        if (out_error != nullptr) {
            *out_error = message;
        }
        return false;
    }

    // 部分协议失败不算致命 —— 比如 AirPlay 的 mDNS 挂了，DLNA 仍然能用。
    if (!mdns_ok || !ssdp_ok) {
        const std::string message = !mdns_error.empty() ? mdns_error : ssdp_error;
        impl_->set_error(message);
        if (out_error != nullptr) {
            *out_error = message;
        }
    }

    impl_->network_monitor = std::thread([this]() { impl_->monitor_loop(); });

    AD_LOG_INFO("服务发现已启动，地址 {}", address);
    return true;
}

void DiscoveryService::stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }

    // 立刻唤醒监控线程，免得 join 要等满一个轮询周期。
    impl_->monitor_wakeup.notify_all();

    if (impl_->network_monitor.joinable()) {
        impl_->network_monitor.join();
    }

    // 先 mDNS 后 SSDP：两者各自都会发出注销报文，让手机端立刻
    // 把设备移出列表，而不是等到超时。
    impl_->withdraw_all();
    impl_->ssdp.stop();

    AD_LOG_INFO("服务发现已停止");
}

bool DiscoveryService::set_device_name(const std::string& name, std::string* out_error) {
    // 名称规则由文档 2.4 定义，这里复用核心的校验，避免两处规则打架。
    const common::DeviceNameValidation validation = common::validate_device_name(name);
    if (!validation.valid) {
        if (out_error != nullptr) {
            *out_error = validation.reason;
        }
        return false;
    }

    std::string address;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->config.device_name == validation.normalized) {
            return true;   // 没变，省掉一次重注册
        }
        impl_->config.device_name = validation.normalized;
        address = impl_->current_address;
    }

    if (!impl_->running.load()) {
        return true;   // 服务没开，只记下新名字，下次 start 会用上
    }

    // 文档 2.4「即时生效」：注销并重新注册，不重启服务。
    // deviceid / uuid / 已配对记录都不变，所以已连接过的手机不需要重新配对。
    AD_LOG_INFO("设备名已改为「{}」，重新注册广播", validation.normalized);

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->withdraw_all();
    if (impl_->config.enable_dlna) {
        impl_->ssdp.stop();
    }

    std::string error;
    impl_->publish_all(address, &error);
    if (impl_->config.enable_dlna) {
        impl_->start_ssdp(address, &error);
    }
    if (!error.empty()) {
        impl_->last_error = error;
    }
    return true;
}

void DiscoveryService::refresh() {
    if (!impl_->running.load()) {
        return;
    }
    std::string address;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        address = impl_->resolve_address();
    }
    impl_->republish(address);
}

bool DiscoveryService::is_running() const {
    return impl_->running.load();
}

std::string DiscoveryService::current_address() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->current_address;
}

std::string DiscoveryService::last_error() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->last_error;
}

}  // namespace adisplay::discovery
