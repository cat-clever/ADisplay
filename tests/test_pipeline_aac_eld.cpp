// ADisplay —— AAC-ELD 的 AudioSpecificConfig 测试
//
// 守的是一处「写错了只表现为没有声音」的地方：镜像伴音的 AAC-ELD 数据里不带
// 任何解码参数，解码器全靠这几个字节。位序错一位就一个样本都解不出来，而日志
// 里只剩一片安静 —— 没有报错、没有异常，只有「没声音」。
//
// 参考值 F8 E8 50 00 是 44100 Hz 立体声 ELD 的规范写法，逐字节钉住它，等于把
// 「对象类型、采样率索引、声道数、帧长标志」四段的位序一次性钉死。
//
// 这套位字段我自己手算过一遍（写测试时先算再写）：把帧长标志当成了 0，
// 算出来是 F8 E6 40，与参考值不符，才发现 ELD 的 480 样本/帧对应的是 1。

#include <adisplay/pipeline/AacEldConfig.h>

#include <cstdint>
#include <vector>

#include "AdTest.h"

namespace {

namespace pipeline = adisplay::pipeline;

using Bytes = std::vector<uint8_t>;

}  // namespace

AD_TEST(pipeline_aac_eld_extradata_reference, "44100 Hz 立体声的 ELD 配置与参考值逐字节一致") {
    const Bytes config = pipeline::build_aac_eld_extradata(44100, 2);
    const Bytes expected = {0xF8, 0xE8, 0x50, 0x00};
    AD_CHECK_EQ(config, expected);
}

AD_TEST(pipeline_aac_eld_extradata_fields, "四段位字段各自落在正确的位置上") {
    const Bytes config = pipeline::build_aac_eld_extradata(44100, 2);
    AD_CHECK_EQ(config.size(), static_cast<std::size_t>(4));
    if (config.size() != 4) {
        return;
    }
    // 前 5 位是转义标记 11111，紧接着 6 位是 39 - 32 = 7。
    AD_CHECK_EQ((config[0] >> 3) & 0x1F, 0x1F);
    AD_CHECK_EQ(((config[0] & 0x07) << 3) | (config[1] >> 5), 7);
    // 采样率索引 4 = 44100；声道数 2。
    AD_CHECK_EQ((config[1] >> 1) & 0x0F, 4);
    AD_CHECK_EQ(((config[1] & 0x01) << 3) | (config[2] >> 5), 2);
    // 帧长标志必须是 1（ELD 的 480 样本/帧）。写成 0 就是一帧都解不出来。
    AD_CHECK_EQ((config[2] >> 4) & 0x01, 1);
}

AD_TEST(pipeline_aac_eld_extradata_48k, "只有采样率索引随采样率变，其余段不动") {
    // 48000 的索引是 3（44100 是 4），所以只差第 2 字节里的那 4 位。
    const Bytes config = pipeline::build_aac_eld_extradata(48000, 2);
    const Bytes expected = {0xF8, 0xE6, 0x50, 0x00};
    AD_CHECK_EQ(config, expected);
}

AD_TEST(pipeline_aac_eld_extradata_mono, "单声道只改声道数那 4 位") {
    // 声道数那 4 位跨了字节边界（`0001` 从第 2 字节的第 0 位起），所以第 3 字节
    // 从 0x50 变成 0x30 —— 手算这段时我把跨字节的位序看漏过一次，这个用例就是
    // 为那一次留下的。镜像伴音实际是立体声，这条只是把位字段的关系钉住。
    const Bytes config = pipeline::build_aac_eld_extradata(44100, 1);
    const Bytes expected = {0xF8, 0xE8, 0x30, 0x00};
    AD_CHECK_EQ(config, expected);
}

int main() {
    return adtest::run_all("AAC-ELD 解码配置测试");
}
