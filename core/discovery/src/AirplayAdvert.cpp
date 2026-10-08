// ADisplay —— AirPlay 广播内容的构造（见 AirplayAdvert.h 的说明）

#include "adisplay/discovery/AirplayAdvert.h"

#include <cstddef>
#include <cstdio>

namespace adisplay::discovery::airplay {
namespace {

// pi 与 UxPlay 的 /info 应答里写的是同一个常量值。
//
// 它本该是每台设备唯一的标识，但协议层的 /info 是写死的，而 /info 与 mDNS
// 必须一致 —— 两边给不同的值，iOS 会认为这是两台设备。所以这里跟着写死，
// 真要用上唯一性，得同时改协议层。
constexpr const char* kPi = "2e388006-13ba-4041-9a67-25dd4a43d536";

std::string format_features() {
    const uint32_t high = static_cast<uint32_t>(kFeatures >> 32);
    const uint32_t low = static_cast<uint32_t>(kFeatures & 0xffffffffULL);

    // 必须与协议层拼这个字串时用的格式逐字符相同（它用的是
    // snprintf("%X")，即大写十六进制、不补前导零）。数值相同而字串不同
    // 同样会让 /info 与广播对不上 —— 这正是本模块存在的意义。
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%X,0x%X", high, low);
    return std::string(buffer);
}

}  // namespace

std::string features_string() {
    static const std::string cached = format_features();
    return cached;
}

std::vector<TxtRecord> airplay_txt(const Advert& advert) {
    if (advert.public_key.empty()) {
        // 不在这里报错：广播本身不该因为缺公钥而整个失败，那样连 DLNA 一起
        // 没了。调用方（AdEngine）会在启动日志里明确说这件事。
        return {
            {"deviceid", advert.device_id},
            {"features", features_string()},
            {"pw", "false"},
            {"flags", "0x4"},
            {"model", advert.model},
            {"pi", kPi},
            {"srcvers", advert.srcvers},
            {"vv", "2"},
        };
    }

    return {
        {"deviceid", advert.device_id},
        {"features", features_string()},
        // pw=false：不用密码。首连确认由我们自己的 PIN 流程负责（文档 6.2），
        // 不走 AirPlay 自带的密码机制 —— 两套确认叠在一起，
        // 用户会看到两个弹窗，而且 AirPlay 那套在接收端是自己实现的，形同虚设。
        {"pw", "false"},
        // flags bit 2（0x4）= 支持音频。
        {"flags", "0x4"},
        {"model", advert.model},
        // 配对校验的关键。iOS 拿它核对接收端身份，缺了它 pair-verify 走不完。
        {"pk", advert.public_key},
        {"pi", kPi},
        {"srcvers", advert.srcvers},
        {"vv", "2"},
    };
}

std::vector<TxtRecord> raop_txt(const Advert& advert) {
    // RAOP 这条记录是给音频流用的，实例名另有格式（<deviceid>@<名字>），
    // 由 DiscoveryService 拼。
    std::vector<TxtRecord> records = {
        {"ch", "2"},
        // cn：支持的音频编码。0=PCM 1=ALAC 2=AAC 3=AAC-ELD。
        {"cn", "0,1,2,3"},
        {"da", "true"},
        // et：支持的加密类型。0=无 3=FairPlay 5=FairPlay SAPv2.5。
        {"et", "0,3,5"},
        {"vv", "2"},
        {"ft", features_string()},
        {"am", advert.model},
        // md：支持的元数据类型（文本/封面/进度）。
        {"md", "0,1,2"},
        {"rhd", "5.6.0.0"},
        {"pw", "false"},
        {"sf", "0x4"},
        {"sr", "44100"},
        {"ss", "16"},
        {"sv", "false"},
        {"tp", "UDP"},
        {"txtvers", "1"},
        {"vs", advert.srcvers},
        {"vn", "65537"},
    };

    if (!advert.public_key.empty()) {
        records.push_back({"pk", advert.public_key});
    }
    return records;
}

std::string txt_wire(const std::vector<TxtRecord>& records) {
    std::string wire;
    for (const TxtRecord& record : records) {
        const std::string entry = record.key + "=" + record.value;
        // 一条 TXT 记录的长度用单个字节表示，放不下就只能丢掉这一条。
        // 静默截断会让协议层的广播与 mDNS 悄悄不一致，所以宁可不发。
        if (entry.size() > 255) {
            continue;
        }
        wire.push_back(static_cast<char>(entry.size()));
        wire.append(entry);
    }
    return wire;
}

}  // namespace adisplay::discovery::airplay
