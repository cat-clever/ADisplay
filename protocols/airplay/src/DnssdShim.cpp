// ADisplay —— AirPlay 协议层与 mDNS 广播之间的接缝
//
// 背景：UxPlay 的协议层从 dnssd_t 里读设备身份（名字、硬件地址、公钥、能力位），
// 既用来拼 /info 应答，也用来拼 TXT 记录。它原本的实现（lib/dnssd.c）依赖
// Apple Bonjour 或 Avahi —— Windows 上两者都没有。而广播本来就是我们自己的
// DiscoveryService 在做，协议层需要的只是「读身份」，所以这里补上这一层。
//
// 这个 shim 的存在是为了消除一类特定的故障：mDNS 通告的内容与 /info 应答的内容
// 不一致。iOS 会因此认为这是两台不同的设备，表现是「列表里看得到、点下去连不上」。
// 具体做法是全部从 AirplayAdvert 取（与 DiscoveryService 同一份），
// 而且设备 id 的格式化直接调用 UxPlay 自己的 utils_hwaddr_airplay ——
// 和 /info 走的是同一段代码，不给自己留写第二遍的机会。
//
// 注意：register / unregister 是空的。广播由 DiscoveryService 负责，
// 这里再注册一次只会让同一条服务出现两遍。

#include "dnssd.h"
#include "DnssdShim.h"

#include <adisplay/discovery/AirplayAdvert.h>

extern "C" {
#include "utils.h"
}

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

// dnssd_t 的 dnssd_private 指向它。UxPlay 不碰这个指针的内容，只负责传回来。
struct ShimPrivate {
    // 返回给协议层的 TXT 数据必须一直有效到 destroy —— 调用方拿到的是裸指针，
    // 而 /info 可能在任意时刻拼应答。所以在这里缓存住，不每次现算。
    std::string airplay_txt;
    std::string raop_txt;

    // 规范化后的设备 id，供 adisplay_dnssd_device_id 用。
    std::string device_id_text;
};

adisplay::discovery::airplay::Advert build_advert(const dnssd_t* dnssd) {
    adisplay::discovery::airplay::Advert advert;

    if (dnssd->name != nullptr && dnssd->name_len > 0) {
        advert.name.assign(dnssd->name, static_cast<std::size_t>(dnssd->name_len));
    }
    if (dnssd->pk != nullptr) {
        advert.public_key = dnssd->pk;
    }

    // 设备 id 的格式化交给 UxPlay 自己那份函数。
    //
    // /info 应答里的 deviceID / macAddress 就是拿同一段原始字节用它转出来的，
    // 所以这里跟着用才能保证两边字字相同。buffer 大小按它的约定给足
    // （每字节写成 "xx:"，末尾多出的那个冒号会被它自己去掉）。
    if (dnssd->hw_addr != nullptr && dnssd->hw_addr_len > 0) {
        const int capacity = 3 * dnssd->hw_addr_len + 1;
        std::vector<char> text(static_cast<std::size_t>(capacity), '\0');
        if (utils_hwaddr_airplay(text.data(), capacity, dnssd->hw_addr,
                                 dnssd->hw_addr_len) > 0) {
            advert.device_id = text.data();
        }
    }

    return advert;
}

// 把缓存好的 TXT 交给调用方。length 必须回填 —— 协议层靠它决定拷贝多少字节，
// 漏填会让它读到越界。
const char* return_txt(const std::string& text, int* length) {
    if (length != nullptr) {
        *length = static_cast<int>(text.size());
    }
    return text.data();
}

}  // namespace

extern "C" {

void* dnssd_private_init(dnssd_t* dnssd_public, int* error) {
    (void) dnssd_public;
    ShimPrivate* priv = new (std::nothrow) ShimPrivate();
    if (priv == nullptr) {
        if (error != nullptr) {
            *error = DNSSD_ERROR_OUTOFMEM;
        }
        return nullptr;
    }
    if (error != nullptr) {
        *error = DNSSD_ERROR_NOERROR;
    }
    return priv;
}

void dnssd_private_destroy(void* dnssd_private) {
    delete static_cast<ShimPrivate*>(dnssd_private);
}

void dnssd_error_text(int* error, const char* appname) {
    (void) appname;
    if (error != nullptr) {
        *error = DNSSD_ERROR_NOERROR;
    }
}

dnssd_t* dnssd_init(const char* name, int name_len, const char* hw_addr, int hw_addr_len,
                    unsigned char pin_pw, int* error) {
    if (name == nullptr || name_len <= 0 || hw_addr == nullptr || hw_addr_len <= 0) {
        if (error != nullptr) {
            *error = DNSSD_ERROR_BADNAME;
        }
        return nullptr;
    }

    dnssd_t* dnssd = static_cast<dnssd_t*>(std::calloc(1, sizeof(dnssd_t)));
    if (dnssd == nullptr) {
        if (error != nullptr) {
            *error = DNSSD_ERROR_OUTOFMEM;
        }
        return nullptr;
    }

    dnssd->name = static_cast<char*>(std::malloc(static_cast<std::size_t>(name_len) + 1));
    dnssd->hw_addr = static_cast<char*>(std::malloc(static_cast<std::size_t>(hw_addr_len)));
    if (dnssd->name == nullptr || dnssd->hw_addr == nullptr) {
        std::free(dnssd->name);
        std::free(dnssd->hw_addr);
        std::free(dnssd);
        if (error != nullptr) {
            *error = DNSSD_ERROR_OUTOFMEM;
        }
        return nullptr;
    }
    std::memcpy(dnssd->name, name, static_cast<std::size_t>(name_len));
    dnssd->name[name_len] = '\0';
    dnssd->name_len = name_len;
    std::memcpy(dnssd->hw_addr, hw_addr, static_cast<std::size_t>(hw_addr_len));
    dnssd->hw_addr_len = hw_addr_len;

    dnssd->pin_pw = pin_pw;
    dnssd->features1 = static_cast<uint32_t>(adisplay::discovery::airplay::kFeatures >> 32);
    dnssd->features2 =
        static_cast<uint32_t>(adisplay::discovery::airplay::kFeatures & 0xffffffffULL);

    dnssd->dnssd_private = dnssd_private_init(dnssd, error);
    if (dnssd->dnssd_private == nullptr) {
        std::free(dnssd->name);
        std::free(dnssd->hw_addr);
        std::free(dnssd);
        return nullptr;
    }

    if (error != nullptr) {
        *error = DNSSD_ERROR_NOERROR;
    }
    return dnssd;
}

// 广播不在这里做（见文件头）。返回 0 表示「成功」——
// 返回别的值会让协议层把「注册失败」记成错误，而实际上广播是好的。
int dnssd_register_raop(dnssd_t* dnssd, unsigned short port) {
    (void) dnssd;
    (void) port;
    return 0;
}

int dnssd_register_airplay(dnssd_t* dnssd, unsigned short port) {
    (void) dnssd;
    (void) port;
    return 0;
}

void dnssd_unregister_raop(dnssd_t* dnssd) {
    (void) dnssd;
}

void dnssd_unregister_airplay(dnssd_t* dnssd) {
    (void) dnssd;
}

const char* dnssd_get_raop_txt(dnssd_t* dnssd, int* length) {
    ShimPrivate* priv = static_cast<ShimPrivate*>(dnssd->dnssd_private);
    priv->raop_txt = adisplay::discovery::airplay::txt_wire(
        adisplay::discovery::airplay::raop_txt(build_advert(dnssd)));
    return return_txt(priv->raop_txt, length);
}

const char* dnssd_get_airplay_txt(dnssd_t* dnssd, int* length) {
    ShimPrivate* priv = static_cast<ShimPrivate*>(dnssd->dnssd_private);
    priv->airplay_txt = adisplay::discovery::airplay::txt_wire(
        adisplay::discovery::airplay::airplay_txt(build_advert(dnssd)));
    return return_txt(priv->airplay_txt, length);
}

const char* dnssd_get_name(dnssd_t* dnssd, int* length) {
    if (length != nullptr) {
        *length = dnssd->name_len;
    }
    return dnssd->name;
}

const char* dnssd_get_hw_addr(dnssd_t* dnssd, int* length) {
    if (length != nullptr) {
        *length = dnssd->hw_addr_len;
    }
    return dnssd->hw_addr;
}

void dnssd_set_airplay_features(dnssd_t* dnssd, int bit, int val) {
    // 只支持 64 位以内的位号；越界直接忽略，而不是让移位行为未定义。
    if (bit < 0 || bit > 63) {
        return;
    }
    uint64_t features = (static_cast<uint64_t>(dnssd->features1) << 32) | dnssd->features2;
    if (val) {
        features |= (1ULL << bit);
    } else {
        features &= ~(1ULL << bit);
    }
    dnssd->features1 = static_cast<uint32_t>(features >> 32);
    dnssd->features2 = static_cast<uint32_t>(features & 0xffffffffULL);
}

uint64_t dnssd_get_airplay_features(dnssd_t* dnssd) {
    return (static_cast<uint64_t>(dnssd->features1) << 32) | dnssd->features2;
}

void dnssd_set_pk(dnssd_t* dnssd, char* pk_str) {
    // 协议层在 raop_set_dnssd 时把公钥交过来，之后 /info 与 TXT 都从这里取。
    if (dnssd == nullptr || pk_str == nullptr) {
        return;
    }
    std::free(dnssd->pk);
    dnssd->pk = static_cast<char*>(std::malloc(std::strlen(pk_str) + 1));
    if (dnssd->pk == nullptr) {
        return;
    }
    std::strcpy(dnssd->pk, pk_str);
}

// 传统 Bonjour/Avahi 实现会交出一个需要应用去读的 socket。我们没有那条连接，
// 按 dnssd.h 的约定返回 -1 表示「无事可做」。
int dnssd_get_service_fd(dnssd_t* dnssd, int service) {
    (void) dnssd;
    (void) service;
    return -1;
}

int dnssd_process_service(dnssd_t* dnssd, int service) {
    (void) dnssd;
    (void) service;
    return 0;
}

void dnssd_destroy(dnssd_t* dnssd) {
    if (dnssd == nullptr) {
        return;
    }
    dnssd_private_destroy(dnssd->dnssd_private);
    std::free(dnssd->name);
    std::free(dnssd->hw_addr);
    std::free(dnssd->pk);
    std::free(dnssd);
}

// ---- 只读窗口（声明见 DnssdShim.h）----------------------------------------

const char* adisplay_dnssd_public_key(dnssd_t* dnssd) {
    if (dnssd == nullptr || dnssd->pk == nullptr) {
        return "";
    }
    return dnssd->pk;
}

const char* adisplay_dnssd_device_id(dnssd_t* dnssd) {
    if (dnssd == nullptr || dnssd->dnssd_private == nullptr) {
        return "";
    }
    ShimPrivate* priv = static_cast<ShimPrivate*>(dnssd->dnssd_private);
    // 缓存一次即可：原始字节到 dnssd_destroy 之前不会变。
    if (priv->device_id_text.empty()) {
        priv->device_id_text = build_advert(dnssd).device_id;
    }
    return priv->device_id_text.c_str();
}

}  // extern "C"
