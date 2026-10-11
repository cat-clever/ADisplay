// ADisplay —— Android 的 mDNS 发布
//
// 这里刻意是「核心不做」的实现，而不是留空或者硬接一个第三方 mDNS 库：
//
// Android 上发布 mDNS 有系统 API（NsdManager），但它是 Java 层的，C++ 这边
// 调不到。而 NDK 里没有等价的公开接口，自己抢 5353 端口实现一份 mDNS 又要
// 和系统的省电策略、MulticastLock 较劲 —— 那是文档 4.5 明确要避免的路子。
//
// 所以分工是：mDNS 由电视端的 Kotlin 用 NsdManager 发布，核心只负责 SSDP
// （DLNA 的发现走 SSDP，不依赖 mDNS）。is_mdns_supported() 如实返回 false，
// 界面层据此知道该自己接手，而不是在一个空洞上猜。
#include <adisplay/discovery/MdnsPublisher.h>

#include <adisplay/common/Log.h>

namespace adisplay::discovery {
namespace {

class MdnsPublisherUnsupported final : public IMdnsPublisher {
public:
    bool publish(const MdnsServiceInfo& info, std::string* out_error) override {
        (void)info;
        const std::string message =
            "Android 上的 mDNS 由电视端用 NsdManager 发布，核心不负责这一层";
        AD_LOG_DEBUG("{}", message);
        if (out_error != nullptr) {
            *out_error = message;
        }
        return false;
    }

    void withdraw() override {}

    bool is_published() const override { return false; }
};

}  // namespace

std::unique_ptr<IMdnsPublisher> create_mdns_publisher() {
    return std::unique_ptr<IMdnsPublisher>(new MdnsPublisherUnsupported());
}

bool is_mdns_supported() {
    return false;
}

}  // namespace adisplay::discovery
