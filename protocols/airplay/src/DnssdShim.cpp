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
// 注意两件事：
//   * dnssd_t 在 dnssd.h 里是不完全类型（typedef struct dnssd_s dnssd_t），
//     所以这个结构体由我们自己定义。协议层只把它当指针传递，不会 sizeof 也不会
//     自己分配，所以用什么定义由我们决定。
//   * register / unregister 是空的。广播由 DiscoveryService 负责，
//     这里再注册一次只会让同一条服务出现两遍。

#include "dnssd.h"
#include "DnssdShim.h"

#include <adisplay/discovery/AirplayAdvert.h>

extern "C" {
#include "global.h"
#include "utils.h"
}

#include <new>
#include <string>
#include <vector>

// dnssd.h 里只声明了 typedef，实体在这里给。
struct dnssd_s {
    std::string name;

    // 原始 MAC 字节。协议层会拿它去格式化 deviceID / macAddress，
    // 所以必须保存字节而不是格式化后的字串。
    std::vector<unsigned char> hw_addr;

    std::string pk;

    uint32_t features1 = 0;
    uint32_t features2 = 0;
    unsigned char pin_pw = 0;

    // 返回给协议层的 TXT 数据必须一直有效到 dnssd_destroy —— 调用方拿到的是裸
    // 指针，而 /info 可能在任意时刻拼应答。所以在这里缓存住，不每次现算。
    std::string airplay_txt;
    std::string raop_txt;

    // 规范化后的设备 id，供 adisplay_dnssd_device_id 用。
    std::string device_id_text;
};

namespace {

adisplay::discovery::airplay::Advert build_advert(const dnssd_t* dnssd) {
    adisplay::discovery::airplay::Advert advert;

    advert.name = dnssd->name;
    advert.public_key = dnssd->pk;
    advert.model = GLOBAL_MODEL;
    advert.srcvers = GLOBAL_VERSION;

    // 设备 id 的格式化交给 UxPlay 自己那份函数。
    //
    // /info 应答里的 deviceID / macAddress 就是拿同一段原始字节用它转出来的，
    // 所以这里跟着用才能保证两边字字相同。缓冲区按它的约定给足
    // （每字节写成 "xx:"，末尾多出的那个冒号会被它自己去掉）。
    if (!dnssd->hw_addr.empty()) {
        const int length = static_cast<int>(dnssd->hw_addr.size());
        const int capacity = 3 * length + 1;
        std::vector<char> text(static_cast<std::size_t>(capacity), '\0');
        if (utils_hwaddr_airplay(text.data(), capacity,
                                 reinterpret_cast<const char*>(dnssd->hw_addr.data()),
                                 length) > 0) {
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

dnssd_t* dnssd_init(const char* name, int name_len, const char* hw_addr, int hw_addr_len,
                    int* error, unsigned char pin_pw) {
    // 硬件地址长度是这里唯一能校验的项 —— dnssd.h 里只有这一条对应的错误码
    // （没有给「名字不合法」的那个）。名字的长度与字符集由上层的设备名校验
    // 保证（文档 2.4），到不了这里。
    if (hw_addr == nullptr || hw_addr_len != MAX_HWADDR_LEN) {
        if (error != nullptr) {
            *error = DNSSD_ERROR_HWADDRLEN;
        }
        return nullptr;
    }

    dnssd_t* dnssd = new (std::nothrow) dnssd_t();
    if (dnssd == nullptr) {
        if (error != nullptr) {
            *error = DNSSD_ERROR_OUTOFMEM;
        }
        return nullptr;
    }

    if (name != nullptr && name_len > 0) {
        dnssd->name.assign(name, static_cast<std::size_t>(name_len));
    }
    dnssd->hw_addr.assign(reinterpret_cast<const unsigned char*>(hw_addr),
                          reinterpret_cast<const unsigned char*>(hw_addr) + hw_addr_len);
    dnssd->pin_pw = pin_pw;

    // 能力位取自共享定义 —— 广播与 /info 用的是同一个值。
    dnssd->features1 = static_cast<uint32_t>(adisplay::discovery::airplay::kFeatures >> 32);
    dnssd->features2 =
        static_cast<uint32_t>(adisplay::discovery::airplay::kFeatures & 0xffffffffULL);

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
    dnssd->raop_txt = adisplay::discovery::airplay::txt_wire(
        adisplay::discovery::airplay::raop_txt(build_advert(dnssd)));
    return return_txt(dnssd->raop_txt, length);
}

const char* dnssd_get_airplay_txt(dnssd_t* dnssd, int* length) {
    dnssd->airplay_txt = adisplay::discovery::airplay::txt_wire(
        adisplay::discovery::airplay::airplay_txt(build_advert(dnssd)));
    return return_txt(dnssd->airplay_txt, length);
}

const char* dnssd_get_name(dnssd_t* dnssd, int* length) {
    if (length != nullptr) {
        *length = static_cast<int>(dnssd->name.size());
    }
    return dnssd->name.c_str();
}

const char* dnssd_get_hw_addr(dnssd_t* dnssd, int* length) {
    if (length != nullptr) {
        *length = static_cast<int>(dnssd->hw_addr.size());
    }
    return reinterpret_cast<const char*>(dnssd->hw_addr.data());
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
    dnssd->pk = pk_str;
}

void dnssd_destroy(dnssd_t* dnssd) {
    delete dnssd;
}

// ---- 只读窗口（声明见 DnssdShim.h）----------------------------------------

const char* adisplay_dnssd_public_key(dnssd_t* dnssd) {
    if (dnssd == nullptr) {
        return "";
    }
    return dnssd->pk.c_str();
}

const char* adisplay_dnssd_device_id(dnssd_t* dnssd) {
    if (dnssd == nullptr) {
        return "";
    }
    // 缓存一次即可：原始字节到 dnssd_destroy 之前不会变。
    if (dnssd->device_id_text.empty()) {
        dnssd->device_id_text = build_advert(dnssd).device_id;
    }
    return dnssd->device_id_text.c_str();
}

const char* adisplay_airplay_model(void) {
    return GLOBAL_MODEL;
}

const char* adisplay_airplay_version(void) {
    return GLOBAL_VERSION;
}

}  // extern "C"
