// ADisplay —— AirPlay 广播内容的测试
//
// 守的是这次修的那类故障：mDNS 通告与协议层 /info 应答不一致。
// 它的表现极具误导性 —— iPhone 的「屏幕镜像」列表里能看到这台电脑，
// 点下去却毫无反应，看日志也只剩「连接失败」几个字，很容易被判成网络问题。
//
// 具体三处必须一致，缺一处就配不上对：
//   * features —— 能力位。取值不对，部分 iOS 版本直接不给镜像入口。
//   * pk       —— 设备公钥。pair-verify 要拿它核对接收端身份。
//   * deviceid —— 设备标识。大小写或格式不同，iOS 会当成两台设备。
//
// 这个文件钉住前两处的字面值。第三处靠「协议层自己算、广播跟着用」
// （见 DnssdShim.cpp 里的 adisplay_dnssd_device_id），在单测里没法覆盖。

#include <adisplay/discovery/AirplayAdvert.h>

#include <map>
#include <string>
#include <vector>

#include "AdTest.h"

namespace {

namespace airplay = adisplay::discovery::airplay;

airplay::Advert make_advert(const std::string& pk) {
    airplay::Advert advert;
    advert.name = "我的电脑";
    advert.device_id = "46:3c:97:d5:d8:3a";
    advert.public_key = pk;
    return advert;
}

std::map<std::string, std::string> to_map(const std::vector<adisplay::discovery::TxtRecord>& records) {
    std::map<std::string, std::string> result;
    for (const adisplay::discovery::TxtRecord& record : records) {
        result[record.key] = record.value;
    }
    return result;
}

}  // namespace

AD_TEST(airplay_features_value, "能力位取值与 UxPlay 一致") {
    // 这个字面值是真机验证过的组合。改动它等于改了 iOS 眼里我们是谁，
    // 所以把它钉死在这里，而不是让它随代码悄悄漂移。
    AD_CHECK_EQ(airplay::features_string(), std::string("0x5A7FFEE6,0x400"));
}

// 这一条守的是一个具体故障：分辨率调高后「投屏窗口闪一下就没了」。
//
// 手机在分辨率较高时会改用 H.265 编码屏幕，而接收端必须先在能力位里声明支持
// （bit 42），否则它发来的参数集包是空的，协议层认不出来就判定编解码器不支持、
// 直接关掉会话 —— 而手机那边仍然显示正在投屏，看起来像是接收端自己掉了。
AD_TEST(airplay_features_h265_bit, "能力位必须声明支持 H.265 镜像（bit 42）") {
    const uint64_t bit42 = static_cast<uint64_t>(1) << 42;
    AD_CHECK_EQ((airplay::kFeatures & bit42), bit42);
    // 它落在高半字：42 = 32 + 10，也就是 features2 的 bit 10。
    //
    // 注意别拿 0x400 去低位里比对：features1 的 bit 10 本来就是 1（0x5A7FFEE6
    // 的低 16 位是 0xFEE6），但那是个无关的能力位。同一个掩码在两个半字里
    // 各有一个位，混起来看会得出错误结论 —— 这里只用上面那条按 bit 42 的断言。
    AD_CHECK_EQ(airplay::kFeatures2, 0x400u);
}

AD_TEST(airplay_features_half_words, "能力位的两个半字不能写反：/info 的整数要跟 TXT 串对得上") {
    // 这一条是补上来的，因为原先的用例只钉了 TXT 字符串 —— 而把两个半字写反时，
    // 字符串照样是对的，只有 /info 里那个 64 位整数不一样。
    //
    // 而 iOS 正是拿 /info 里那个整数决定要不要走屏幕镜像这条路的。写反的表现是
    // 「手机能搜到、连得上、配对与 FairPlay 全部成功，然后不发流」——
    // 协议层日志里一切正常，只有逐字节比对 /info 的应答才看得出来。
    AD_CHECK_EQ(airplay::kFeatures1, 0x5A7FFEE6u);
    AD_CHECK_EQ(airplay::kFeatures2, 0x400u);

    // 协议层 dnssd_get_airplay_features 返回的是 (features2 << 32) | features1。
    // 这里逐字复刻那个表达式，等于把两边钉在一起：谁改了约定都会红。
    AD_CHECK_EQ(airplay::kFeatures,
                (static_cast<uint64_t>(airplay::kFeatures2) << 32) | airplay::kFeatures1);
    // 换成人话：0x5A7FFEE6 必须落在**低** 32 位上。
    AD_CHECK_EQ(airplay::kFeatures, static_cast<uint64_t>(0x4005A7FFEE6));

    // 字符串里第一个数是低半字 —— 与协议层 snprintf("%X,%X", features1, features2) 一致。
    AD_CHECK_EQ(airplay::features_string(), std::string("0x5A7FFEE6,0x400"));
}

AD_TEST(airplay_txt_carries_public_key, "_airplay._tcp 必须带 pk") {
    const std::map<std::string, std::string> txt =
        to_map(airplay::airplay_txt(make_advert("aabbccdd")));

    // 少了这一条，iPhone 能发现我们但走不完配对 —— 也就是「看得见、连不上」。
    AD_CHECK(txt.find("pk") != txt.end());
    AD_CHECK_EQ(txt.at("pk"), std::string("aabbccdd"));
    AD_CHECK_EQ(txt.at("deviceid"), std::string("46:3c:97:d5:d8:3a"));
    AD_CHECK_EQ(txt.at("features"), std::string("0x5A7FFEE6,0x400"));
    // 不用 AirPlay 自带的密码：首连确认由我们自己的流程负责，
    // 两套确认叠在一起对用户是纯干扰。
    AD_CHECK_EQ(txt.at("pw"), std::string("false"));
    // flags 的 bit 2 表示支持音频。置 0 会让部分 iOS 认为这是纯视频接收端。
    AD_CHECK_EQ(txt.at("flags"), std::string("0x4"));
}

AD_TEST(airplay_txt_without_key_stays_valid, "公钥缺失时广播仍然合法，只是没有 pk") {
    const std::map<std::string, std::string> txt = to_map(airplay::airplay_txt(make_advert("")));

    // 缺公钥不是广播失败。让它照常公布、只是不发 pk —— 这样设备仍然
    // 可见，配合启动日志里的告警，能一眼看出问题在密钥而不在网络。
    AD_CHECK(txt.find("pk") == txt.end());
    AD_CHECK_EQ(txt.at("features"), std::string("0x5A7FFEE6,0x400"));
    AD_CHECK_EQ(txt.at("deviceid"), std::string("46:3c:97:d5:d8:3a"));
}

AD_TEST(raop_txt_carries_required_entries, "_raop._tcp 带齐音频协商所需的记录") {
    const std::map<std::string, std::string> txt = to_map(airplay::raop_txt(make_advert("1234")));

    AD_CHECK_EQ(txt.at("ch"), std::string("2"));
    AD_CHECK_EQ(txt.at("cn"), std::string("0,1,2,3"));
    AD_CHECK_EQ(txt.at("et"), std::string("0,3,5"));
    AD_CHECK_EQ(txt.at("sr"), std::string("44100"));
    AD_CHECK_EQ(txt.at("ss"), std::string("16"));
    AD_CHECK_EQ(txt.at("tp"), std::string("UDP"));
    AD_CHECK_EQ(txt.at("vn"), std::string("65537"));
    // txtvers 与 sf 是 RAOP 侧的必填项：旧版客户端缺了 txtvers 会直接跳过
    // 这条记录，于是音频整条路径不成立。
    AD_CHECK_EQ(txt.at("txtvers"), std::string("1"));
    AD_CHECK_EQ(txt.at("sf"), std::string("0x4"));
    AD_CHECK_EQ(txt.at("pw"), std::string("false"));
    // ft 是 RAOP 侧的能力位，必须与 _airplay._tcp 的 features 同一个值。
    AD_CHECK_EQ(txt.at("ft"), std::string("0x5A7FFEE6,0x400"));
    AD_CHECK_EQ(txt.at("pk"), std::string("1234"));
}

AD_TEST(txt_wire_is_length_prefixed, "线上格式：每条为长度字节加 key=value") {
    const std::vector<adisplay::discovery::TxtRecord> records = {
        {"a", "1"},
        {"bb", "22"},
    };
    const std::string wire = airplay::txt_wire(records);

    // "\x03a=1" + "\x05bb=22"
    AD_CHECK_EQ(wire.size(), static_cast<std::size_t>(4 + 6));
    AD_CHECK_EQ(static_cast<int>(static_cast<unsigned char>(wire[0])), 3);
    AD_CHECK_EQ(wire.substr(1, 3), std::string("a=1"));
    AD_CHECK_EQ(static_cast<int>(static_cast<unsigned char>(wire[4])), 5);
    AD_CHECK_EQ(wire.substr(5, 5), std::string("bb=22"));
}

AD_TEST(txt_wire_skips_oversized_entries, "超长的一条宁可丢掉也不截断") {
    // 单条 TXT 的长度是一个字节。截断会让广播与 /info 悄悄不一致 ——
    // 那正是这个模块要消灭的故障，所以宁可少一条。
    const std::vector<adisplay::discovery::TxtRecord> records = {
        {"ok", "1"},
        {"big", std::string(300, 'x')},
    };
    const std::string wire = airplay::txt_wire(records);

    AD_CHECK_EQ(wire.size(), static_cast<std::size_t>(1 + 4));
    AD_CHECK_EQ(wire.substr(1, 4), std::string("ok=1"));
}

AD_TEST(raop_txt_wire_round_trips, "整条 RAOP 记录能从线上格式原样解回来") {
    const std::string wire = airplay::txt_wire(airplay::raop_txt(make_advert("abcd")));

    std::map<std::string, std::string> parsed;
    std::size_t offset = 0;
    while (offset < wire.size()) {
        const std::size_t length = static_cast<unsigned char>(wire[offset]);
        ++offset;
        AD_CHECK(offset + length <= wire.size());
        const std::string entry = wire.substr(offset, length);
        offset += length;

        const std::size_t equal = entry.find('=');
        AD_CHECK(equal != std::string::npos);
        parsed[entry.substr(0, equal)] = entry.substr(equal + 1);
    }

    // 协议层就是按这个格式把 TXT 交给 /info 的，所以能解回来才算构造正确。
    AD_CHECK_EQ(parsed.size(), static_cast<std::size_t>(19));
    AD_CHECK_EQ(parsed.at("pk"), std::string("abcd"));
    AD_CHECK_EQ(parsed.at("am"), std::string("AppleTV3,2"));
}

// 这个格式是**跨语言的契约**：Android 侧（Kotlin）按行切开、把十六进制解回
// 字节，再交给 NsdManager。格式一变两边就对不上，而症状是「iPhone 里看不到
// 这台设备」—— 和广播内容出错时一样难查。
AD_TEST(airplay_advert_for_platform, "给平台的广播描述：行格式稳定，TXT 值一律十六进制") {
    airplay::Advert advert = make_advert("abcd");
    const std::string text = airplay::advert_for_platform(advert, 7001);

    // 按行切开 —— 与 Android 侧的做法一致。
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    AD_CHECK(lines.size() >= 3);
    if (lines.size() < 3) {
        return;
    }

    AD_CHECK_EQ(lines[0], std::string("port=7001"));
    // 名字是 UTF-8 原文，不做转义 —— 中文设备名要能原样过去。
    AD_CHECK_EQ(lines[1], std::string("airplay_name=我的电脑"));
    // RAOP 的实例名必须是 <deviceid>@<名字>：iOS 靠它把音频流关联回 AirPlay
    // 那条记录，少了 @ 或大小写不同就会当成两台设备。
    AD_CHECK_EQ(lines[2], std::string("raop_name=46:3c:97:d5:d8:3a@我的电脑"));

    // TXT 的值全部十六进制：pk 是设备公钥（二进制），am/vs 是纯文本，
    // 统一编码省得界面层分辨。
    const std::string pk_line = "airplay.pk=" + std::string("61626364");
    bool found_pk = false;
    for (const std::string& line : lines) {
        if (line == pk_line) {
            found_pk = true;
        }
    }
    AD_CHECK(found_pk);
}

int main() {
    return adtest::run_all("AirPlay 广播内容测试");
}
