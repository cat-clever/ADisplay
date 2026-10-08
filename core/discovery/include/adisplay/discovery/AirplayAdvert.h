// ADisplay —— AirPlay 广播内容的唯一出处
//
// mDNS 通告的 TXT 记录与 /info 应答必须完全一致。设备公钥、能力位、型号
// 任何一处对不上，iOS 的表现都是一样的：列表里看得到这台设备，点下去连不上。
// 这是 AirPlay 对接里最容易被误判成网络问题的一类故障，所以两份内容不各写各的 ——
// 都从这里取，改值只改这一处。
//
// 取值对齐 UxPlay（真机验证过的一组）。尤其是 _raop._tcp 的 pk：
// iOS 的 pair-verify 要用它对接收端的密钥做校验，缺了它配对流就走不完。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "MdnsPublisher.h"

namespace adisplay::discovery::airplay {

// 广播所需的设备身份。
//
// public_key 由 AirPlay 协议层生成（Ed25519），我们只负责如实转播 ——
// 它必须和接收端实际持有的私钥配对，所以不能由这一层自己造。
struct Advert {
    // 显示名，即手机投屏列表里看到的名字（文档 2.4）。
    std::string name;

    // MAC 风格的设备 ID。同时也是 _raop._tcp 实例名的前缀（<id>@<名字>），
    // iOS 靠它把 RAOP 音频流关联回 _airplay._tcp 那条记录。
    std::string device_id;

    // Ed25519 公钥的十六进制串（小写，无分隔）。空表示协议层还没就绪，
    // 此时广播会缺 pk —— 服务能起来，但配对必然失败，所以这种情况要记日志。
    std::string public_key;

    // AirPlay 的 model 与 srcvers。
    std::string model = "AppleTV6,2";
    std::string srcvers = "220.68";
};

// 能力位（features）。
//
// 与 UxPlay 一致。其中 bit 27（0x08000000）表示「支持传统配对」，关掉它
// 部分 iOS 版本会连「屏幕镜像」入口都不给。第二位 0x0 表示不声明
// AirPlay 2 多房间等我们用不上的能力 —— 声明了做不到的能力，
// 手机的连接流程会走到我们接不住的分支上。
constexpr uint64_t kFeatures = (0x5A7FFEE6ULL << 32) | 0x0ULL;

// features 的字串形态，形如 "0x5A7FFEE6,0x0"。TXT 记录与 /info 都用它。
std::string features_string();

std::vector<TxtRecord> airplay_txt(const Advert& advert);
std::vector<TxtRecord> raop_txt(const Advert& advert);

// TXT 记录的 DNS 线上格式：每条是「一个长度字节 + "key=value"」，首尾相接。
// /info 应答里的 txtAirPlay / txtRAOP 两个字段要的正是这个形态，
// 而 mDNS 由各平台系统 API 负责编码，所以只有协议层用得到它。
std::string txt_wire(const std::vector<TxtRecord>& records);

}  // namespace adisplay::discovery::airplay
