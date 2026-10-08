// ADisplay —— Linux 的 mDNS 发布实现（暂缓）
//
// 文档 1.3 把 Linux 桌面端标为 P2「暂缓」：界面层（GTK 4）不做，
// 但核心库仍然要在 Linux 上编译并跑单元测试，用于及早发现可移植性问题。
//
// 所以这个文件提供的是一个「明确不支持」的实现，而不是留空 ——
// 让 is_mdns_supported() 能如实返回 false，界面层据此给出提示，
// 而不是让调用方在一个空洞上猜测。
//
// 将来接入时：用 Avahi 的 libdns_sd 兼容层（avahi-compat-libdnsd），
// 它导出的 DNSServiceRegister 等符号与 macOS 的 dns_sd.h 一致，
// 可以直接复用 platform/macos 的那份实现，只换链接的库。
#include <adisplay/discovery/MdnsPublisher.h>

#include <adisplay/common/Log.h>

namespace adisplay::discovery {
namespace {

class MdnsPublisherUnsupported final : public IMdnsPublisher {
public:
    bool publish(const MdnsServiceInfo& info, std::string* out_error) override {
        (void)info;
        const std::string message =
            "当前平台尚未实现 mDNS 发布（Linux 支持在文档中标记为 P2 暂缓）";
        AD_LOG_WARN("{}", message);
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
