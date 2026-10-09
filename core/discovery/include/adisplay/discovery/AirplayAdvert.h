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
    //
    // 默认值对齐 UxPlay 这个标签版的 GLOBAL_MODEL / GLOBAL_VERSION。生产路径上
    // 由 AdEngine 从协议层取真值填进来 —— 这里的默认值只在单测与「协议层没起来」
    // 这类边界上用得到。
    std::string model = "AppleTV3,2";
    std::string srcvers = "220.68";
};

// 能力位（features），拆成两个 32 位半字。
//
// 约定与协议层一致，而且**半字的高低顺序不能弄反**：
//   features1 是**低** 32 位，features2 是**高** 32 位，
//   /info 里那个 64 位整数 = (features2 << 32) | features1。
// mDNS 的 TXT 字符串则按「features1,features2」的顺序拼。
//
// 写反的后果非常隐蔽：TXT 字符串仍然对得上，只有 /info 里的整数不一样，
// 而 iOS 正是拿那个整数决定要不要走屏幕镜像这条路的。现象是「手机能搜到、
// 连得上、配对与 FairPlay 全部成功，然后不发流」—— 协议层日志里一切正常，
// 只有把 /info 的应答逐字节对比才看得出来。我们踩过一次。
constexpr uint32_t kFeatures1 = 0x5A7FFEE6;   // 低位：bit 27（支持传统配对）等
// 高位只有 bit 42：Supports Screen Multi Codec，也就是「支持 H.265 镜像」。
//
// 这个位不能省。手机在分辨率较高时会改用 H.265 编码屏幕，而位没置上时它发来的
// 参数集包是**空的** —— 协议层认不出来，就判定编解码器不支持并直接关掉会话。
// 现象是「投屏窗口闪一下就没了，手机上却还显示正在投屏」。协议层日志里的原话是
//   received type 0x01 packet with no payload:
//   this indicates non-h264 video but Airplay features bit 42 is not set
constexpr uint32_t kFeatures2 = 0x400;

// 协议层 /info 应答里那个 64 位整数。
constexpr uint64_t kFeatures = (static_cast<uint64_t>(kFeatures2) << 32) | kFeatures1;

// features 的字串形态，形如 "0x5A7FFEE6,0x0"。TXT 记录与 /info 都用它。
std::string features_string();

std::vector<TxtRecord> airplay_txt(const Advert& advert);
std::vector<TxtRecord> raop_txt(const Advert& advert);

// TXT 记录的 DNS 线上格式：每条是「一个长度字节 + "key=value"」，首尾相接。
// /info 应答里的 txtAirPlay / txtRAOP 两个字段要的正是这个形态，
// 而 mDNS 由各平台系统 API 负责编码，所以只有协议层用得到它。
std::string txt_wire(const std::vector<TxtRecord>& records);

}  // namespace adisplay::discovery::airplay
