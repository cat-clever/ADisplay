// ADisplay —— macOS 的 mDNS 发布实现（系统 Bonjour）
//
// 文档 4.5：macOS 用系统自带 Bonjour（dns_sd.h）。
// macOS 上 mDNSResponder 本来就常驻，走它的 API 是唯一稳妥的做法 ——
// 自己绑 5353 会与系统服务抢组播端口。
#include <adisplay/discovery/MdnsPublisher.h>

#include <adisplay/common/Log.h>

#include <dns_sd.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

namespace adisplay::discovery {
namespace {

// TXT 记录里单个值最长 255 字节，超了 TXTRecordSetValue 会返回错误。
constexpr std::size_t kMaxTxtValueLength = 255;

// 事件循环里 select 的超时。取小一点是为了让停止请求能及时被看到 ——
// 用阻塞式 select 的话，注销要等到下一次有报文进来才会执行。
constexpr int kSelectTimeoutUsec = 200 * 1000;

class MdnsPublisherMac final : public IMdnsPublisher {
public:
    MdnsPublisherMac() = default;

    ~MdnsPublisherMac() override {
        withdraw();
    }

    bool publish(const MdnsServiceInfo& info, std::string* out_error) override {
        // 改名走的就是这条路径：先撤掉旧的再登记新的。
        withdraw();

        if (info.service_type.empty()) {
            set_error(out_error, "服务类型为空");
            return false;
        }
        if (info.instance_name.empty()) {
            set_error(out_error, "实例名（设备名）为空");
            return false;
        }
        if (info.port == 0) {
            set_error(out_error, "服务端口为 0");
            return false;
        }

        TXTRecordRef txt;
        TXTRecordCreate(&txt, 0, nullptr);

        for (const TxtRecord& entry : info.txt) {
            if (entry.key.empty() || entry.key.size() > kMaxTxtValueLength) {
                continue;   // 键非法就跳过，不让一条坏记录毁掉整次注册
            }
            if (entry.value.size() > kMaxTxtValueLength) {
                AD_LOG_WARN("mDNS TXT 值过长已跳过：{}", entry.key);
                continue;
            }
            const DNSServiceErrorType err = TXTRecordSetValue(
                &txt, entry.key.c_str(),
                static_cast<uint8_t>(entry.value.size()),
                entry.value.empty() ? "" : entry.value.data());
            if (err != kDNSServiceErr_NoError) {
                AD_LOG_WARN("mDNS TXT 写入失败：{}（错误码 {}）", entry.key, static_cast<int>(err));
            }
        }

        DNSServiceRef ref = nullptr;
        const char* host = info.host_name.empty() ? nullptr : info.host_name.c_str();

        const DNSServiceErrorType error = DNSServiceRegister(
            &ref,
            0,                                  // flags
            kDNSServiceInterfaceIndexAny,       // 所有网卡
            info.instance_name.c_str(),
            info.service_type.c_str(),
            nullptr,                            // 默认 "local."
            host,
            htons(info.port),
            TXTRecordGetLength(&txt),
            TXTRecordGetBytesPtr(&txt),
            nullptr,                            // 不需要注册回调
            nullptr);

        TXTRecordDeallocate(&txt);

        if (error != kDNSServiceErr_NoError) {
            // -65537 之类的错误码在 macOS 上常见于缺少本地网络权限或沙箱限制。
            const std::string message =
                "mDNS 注册失败（错误码 " + std::to_string(static_cast<int>(error)) + "）：" +
                info.service_type;
            AD_LOG_ERROR("{}", message);
            set_error(out_error, message);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            ref_ = ref;
            service_type_ = info.service_type;
            instance_name_ = info.instance_name;
        }

        // DNSServiceRegister 是异步的，必须有人持续调用 DNSServiceProcessResult
        // 它才会真正把报文发出去 —— 注册完就不管的话，手机上什么也看不到。
        running_.store(true);
        worker_ = std::thread([this]() { run_loop(); });

        AD_LOG_INFO("mDNS 已公布 {} / {} 端口 {}", info.service_type, info.instance_name, info.port);
        return true;
    }

    void withdraw() override {
        running_.store(false);

        if (worker_.joinable()) {
            worker_.join();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (ref_ != nullptr) {
            // DNSServiceRefDeallocate 会顺带发出 goodbye 包，
            // 手机端列表里立刻就能看到设备消失。
            DNSServiceRefDeallocate(ref_);
            ref_ = nullptr;
        }
        service_type_.clear();
        instance_name_.clear();
    }

    bool is_published() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return ref_ != nullptr;
    }

private:
    void run_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            DNSServiceRef ref = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ref = ref_;
            }
            if (ref == nullptr) {
                return;
            }

            const int fd = DNSServiceRefSockFD(ref);
            if (fd < 0) {
                return;
            }

            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(fd, &read_set);

            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = kSelectTimeoutUsec;

            const int ready = ::select(fd + 1, &read_set, nullptr, nullptr, &timeout);

            if (!running_.load(std::memory_order_relaxed)) {
                return;
            }
            if (ready <= 0) {
                continue;   // 超时或被打断，回到循环顶部检查停止标志
            }
            if (!FD_ISSET(fd, &read_set)) {
                continue;
            }

            DNSServiceProcessResult(ref);
        }
    }

    void set_error(std::string* out_error, const std::string& message) {
        if (out_error != nullptr) {
            *out_error = message;
        }
    }

    mutable std::mutex mutex_;
    DNSServiceRef ref_ = nullptr;
    std::string service_type_;
    std::string instance_name_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

}  // namespace

std::unique_ptr<IMdnsPublisher> create_mdns_publisher() {
    return std::unique_ptr<IMdnsPublisher>(new MdnsPublisherMac());
}

bool is_mdns_supported() {
    return true;
}

}  // namespace adisplay::discovery
