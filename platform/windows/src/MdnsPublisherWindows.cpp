// ADisplay —— Windows 的 mDNS 发布实现（系统 DNS-SD API）
//
// 文档 4.5：Windows 走系统 DNS-SD API。对应的是 windns.h 的 DnsServiceRegister，
// 需要 Windows 10 1703（创意者更新）及以上，且 DNS Client 服务在运行。
//
// 与 macOS 的 dns_sd.h 有两点不同，容易踩：
//   1. 这里的实例名要传「实例名.服务类型.local」的全名，而不是分开传两段。
//   2. 接口全是宽字符，UTF-8 的设备名必须先转成 UTF-16，否则中文设备名会乱码。
#include <adisplay/discovery/MdnsPublisher.h>

#include <adisplay/common/Log.h>

#include <windows.h>
#include <windns.h>

// CoTaskMemAlloc / CoTaskMemFree 声明在这里。
// 必须显式包含：项目全局开了 WIN32_LEAN_AND_MEAN（见顶层 CMakeLists），
// 它会把 <windows.h> 里的 COM 头一并裁掉，这两个函数就找不到了。
#include <combaseapi.h>

#include <atomic>
#include <cstring>   // memcpy / memset
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace adisplay::discovery {
namespace {

// Windows 的 TXT 值用宽字符串，且长度受 DNS 报文限制。
constexpr std::size_t kMaxTxtValueLength = 255;

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int length = ::MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          wide.data(), length);
    return wide;
}

// 宽字符串的所有权由调用方负责，用这个 RAII 包一下。
class WideStringPool {
public:
    ~WideStringPool() {
        for (wchar_t* pointer : owned_) {
            ::CoTaskMemFree(pointer);
        }
    }

    // 复制一份宽字符串到独立内存。返回的指针在池子析构前一直有效 ——
    // DnsServiceRegister 是异步的，调用返回后它仍会读这些内存，
    // 所以不能传指向临时 std::wstring 的指针。
    wchar_t* intern(const std::wstring& text) {
        const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        wchar_t* copy = static_cast<wchar_t*>(::CoTaskMemAlloc(bytes));
        if (copy == nullptr) {
            return nullptr;
        }
        std::memcpy(copy, text.c_str(), bytes);
        owned_.push_back(copy);
        return copy;
    }

private:
    std::vector<wchar_t*> owned_;
};

class MdnsPublisherWindows final : public IMdnsPublisher {
public:
    MdnsPublisherWindows() = default;

    ~MdnsPublisherWindows() override {
        withdraw();
    }

    bool publish(const MdnsServiceInfo& info, std::string* out_error) override {
        withdraw();

        if (info.service_type.empty() || info.instance_name.empty() || info.port == 0) {
            set_error(out_error, "服务类型、实例名或端口不合法");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pool_ = std::make_unique<WideStringPool>();

            // Windows 要求完整名称：<实例名>.<服务类型>.local
            // 注意服务类型是 "_airplay._tcp" 这种带下划线和点号的形式。
            std::wstring full_name = utf8_to_wide(info.instance_name);
            full_name += L".";
            full_name += utf8_to_wide(info.service_type);
            full_name += L".local";

            instance_name_ = pool_->intern(full_name);
            if (instance_name_ == nullptr) {
                set_error(out_error, "内存不足");
                return false;
            }

            // TXT 记录：键值各一个数组。DPAPI 之前先把字符串拷进池子。
            keys_.clear();
            values_.clear();
            for (const TxtRecord& entry : info.txt) {
                if (entry.key.empty() || entry.value.size() > kMaxTxtValueLength) {
                    continue;
                }
                wchar_t* key = pool_->intern(utf8_to_wide(entry.key));
                wchar_t* value = pool_->intern(utf8_to_wide(entry.value));
                if (key == nullptr || value == nullptr) {
                    continue;
                }
                keys_.push_back(key);
                values_.push_back(value);
            }

            // pszHostName 是必填的。原来这里留了 nullptr，注释写「由系统取本机主机名」——
            // 那是错的：DNS-SD 拿这个字段去发主机的 A 记录，系统不会替我们补，留空就是
            // 注册失败。取本机主机名，再按 mDNS 的惯例补上 .local。
            wchar_t host_buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
            DWORD host_length = MAX_COMPUTERNAME_LENGTH + 1;
            if (::GetComputerNameExW(ComputerNameDnsHostname, host_buffer, &host_length) == FALSE) {
                set_error(out_error, "取本机主机名失败（GetComputerNameExW 出错）");
                return false;
            }
            std::wstring host_name(host_buffer, host_length);
            if (host_name.find(L'.') == std::wstring::npos) {
                host_name += L".local";
            }
            host_name_ = pool_->intern(host_name);
            if (host_name_ == nullptr) {
                set_error(out_error, "内存不足（主机名）");
                return false;
            }

            std::memset(&instance_, 0, sizeof(instance_));
            instance_.pszInstanceName = instance_name_;
            instance_.pszHostName = host_name_;
            instance_.wPort = info.port;          // 注意是主机字节序，不是网络字节序
            instance_.wPriority = 0;
            instance_.wWeight = 0;
            instance_.dwPropertyCount = static_cast<DWORD>(keys_.size());
            instance_.keys = keys_.empty() ? nullptr : keys_.data();
            instance_.values = values_.empty() ? nullptr : values_.data();
            instance_.dwInterfaceIndex = 0;       // 0 = 所有网卡

            std::memset(&request_, 0, sizeof(request_));
            request_.Version = DNS_QUERY_REQUEST_VERSION1;
            request_.pServiceInstance = &instance_;
            request_.pRegisterCompletionCallback = &MdnsPublisherWindows::on_registered;
            request_.pQueryContext = this;
            request_.hCredentials = nullptr;
            request_.unicastEnabled = FALSE;
        }

        const DWORD status = ::DnsServiceRegister(&request_, &cancel_handle_);
        if (status == DNS_REQUEST_PENDING || status == ERROR_SUCCESS) {
            has_cancel_handle_ = true;
        } else {
            // 把系统对这个错误码的描述一并打出来。
            //
            // 原来这里写的是一句猜测（"需要 Windows 10 1703 以上、DNS Client 服务
            // 在运行"），而实测返回的 14 在 Win32 里是 ERROR_OUTOFMEMORY，与那句话
            // 对不上 —— 猜出来的提示会把排查方向带偏。让系统自己说，是什么就是什么。
            // 用宽字符版本再转 UTF-8。FormatMessageA 给的是系统 ANSI 编码（中文
            // Windows 上是 GBK），直接当 UTF-8 写进日志就是一片乱码 —— 第一版就是
            // 这么写的，日志里看到的是一串问号方块。
            LPWSTR wide_buffer = nullptr;
            const DWORD wide_length = ::FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
                nullptr, status, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                reinterpret_cast<LPWSTR>(&wide_buffer), 0, nullptr);

            std::string detail;
            if (wide_length > 0 && wide_buffer != nullptr) {
                const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, wide_buffer,
                                                        static_cast<int>(wide_length),
                                                        nullptr, 0, nullptr, nullptr);
                if (bytes > 0) {
                    detail.resize(static_cast<std::size_t>(bytes));
                    ::WideCharToMultiByte(CP_UTF8, 0, wide_buffer,
                                          static_cast<int>(wide_length), &detail[0], bytes,
                                          nullptr, nullptr);
                }
                ::LocalFree(wide_buffer);
                while (!detail.empty() &&
                       (detail.back() == '\n' || detail.back() == '\r' || detail.back() == ' ')) {
                    detail.pop_back();
                }
            }
            if (detail.empty()) {
                // DNS-SD 的 DNS_ERROR_* 码在 9500 以上，未必在系统消息表里。
                detail = "系统没有给出这个错误码的描述";
            }

            const std::string message =
                "mDNS 注册失败（错误码 " + std::to_string(static_cast<unsigned long>(status)) +
                "：" + detail + "）";
            AD_LOG_ERROR("{}", message);
            set_error(out_error, message);
            withdraw();
            return false;
        }

        published_.store(true);
        AD_LOG_INFO("mDNS 已公布 {} / {} 端口 {}",
                    info.service_type, info.instance_name, info.port);
        return true;
    }

    void withdraw() override {
        if (!published_.exchange(false)) {
            // 没登记过也要把上一次的池子清掉。
            std::lock_guard<std::mutex> lock(mutex_);
            pool_.reset();
            return;
        }

        if (has_cancel_handle_) {
            ::DnsServiceDeRegister(&request_, &cancel_handle_);
            has_cancel_handle_ = false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        pool_.reset();
        keys_.clear();
        values_.clear();
        instance_name_ = nullptr;
        std::memset(&instance_, 0, sizeof(instance_));
    }

    bool is_published() const override {
        return published_.load();
    }

private:
    // 注册是异步的，完成（或失败）时系统会回调这里。
    // 成功的注册不会有额外动作 —— 系统已把记录放进了响应缓存。
    static void WINAPI on_registered(DWORD status, PVOID context,
                                     PDNS_SERVICE_INSTANCE instance) {
        auto* self = static_cast<MdnsPublisherWindows*>(context);
        if (self == nullptr) {
            return;
        }
        if (status != ERROR_SUCCESS && status != DNS_REQUEST_PENDING) {
            AD_LOG_WARN("mDNS 注册回调报告失败，错误码 {}", static_cast<unsigned long>(status));
            self->published_.store(false);
        }
    }

    void set_error(std::string* out_error, const std::string& message) {
        if (out_error != nullptr) {
            *out_error = message;
        }
    }

    mutable std::mutex mutex_;
    std::unique_ptr<WideStringPool> pool_;
    DNS_SERVICE_INSTANCE instance_{};
    DNS_SERVICE_REGISTER_REQUEST request_{};
    // DNS_SERVICE_CANCEL 是 struct（内含一个 PVOID），不是指针 ——
    // 不能拿它和 nullptr 比较，所以另用一个标志位记「有没有注册句柄」。
    DNS_SERVICE_CANCEL cancel_handle_{};
    bool has_cancel_handle_ = false;
    wchar_t* instance_name_ = nullptr;
    wchar_t* host_name_ = nullptr;
    std::vector<wchar_t*> keys_;
    std::vector<wchar_t*> values_;
    std::atomic<bool> published_{false};
};

}  // namespace

std::unique_ptr<IMdnsPublisher> create_mdns_publisher() {
    return std::unique_ptr<IMdnsPublisher>(new MdnsPublisherWindows());
}

bool is_mdns_supported() {
    return true;
}

}  // namespace adisplay::discovery
