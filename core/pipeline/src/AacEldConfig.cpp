#include <adisplay/pipeline/AacEldConfig.h>

namespace adisplay {
namespace pipeline {
namespace {

// AudioSpecificConfig 是按位写的（大端）。用一个小位写入器而不是手工拼字节 ——
// 手工拼过一次，把 ELD 的帧长标志写成了 0，结果整条路静默无声。
void put_bits(std::vector<uint8_t>& out, uint32_t value, int bits, int* bit_pos) {
    for (int i = bits - 1; i >= 0; --i) {
        const int byte_index = *bit_pos / 8;
        const int bit_index = 7 - (*bit_pos % 8);
        if (byte_index >= static_cast<int>(out.size())) {
            out.push_back(0);
        }
        // 移位量用无符号：这里只可能是 0..31，而有符号移位会触发
        // -Wsign-conversion（Android 的 NDK 把这组警告打开了）。
        if (((value >> static_cast<unsigned>(i)) & 1u) != 0u) {
            out[byte_index] =
                static_cast<uint8_t>(out[byte_index] | (1u << static_cast<unsigned>(bit_index)));
        }
        ++(*bit_pos);
    }
}

// 采样率在配置里是一个 4 位的索引，不是具体数值。
int sampling_frequency_index(int sample_rate) {
    switch (sample_rate) {
        case 96000: return 0;
        case 88200: return 1;
        case 64000: return 2;
        case 48000: return 3;
        case 44100: return 4;
        case 32000: return 5;
        case 24000: return 6;
        case 22050: return 7;
        case 16000: return 8;
        case 12000: return 9;
        case 11025: return 10;
        case 8000:  return 11;
        case 7350:  return 12;
        default:    return -1;
    }
}

}  // namespace

std::vector<uint8_t> build_aac_eld_extradata(int sample_rate, int channels) {
    // AAC-ELD 的对象类型是 39。它超出 5 位的表示范围（最大 31），所以按规范
    // 写成两段：「11111」表示「类型在下面 6 位」，再跟 39 - 32 = 7。
    constexpr uint32_t kEldAudioObjectType = 39;
    constexpr uint32_t kEscapeValue = 31;

    const int index = sampling_frequency_index(sample_rate);
    const uint32_t frequency_bits = static_cast<uint32_t>(index < 0 ? 4 : index);

    std::vector<uint8_t> out;
    int bit = 0;
    put_bits(out, kEscapeValue, 5, &bit);
    put_bits(out, kEldAudioObjectType - 32, 6, &bit);
    put_bits(out, frequency_bits, 4, &bit);
    put_bits(out, static_cast<uint32_t>(channels), 4, &bit);
    // ELDSpecificConfig 的第一位是帧长标志：ELD 下 1 表示 480 样本/帧，
    // 0 表示 512。镜像伴音是 480，所以这里是 1 —— 写成 0 就一帧都解不出来。
    put_bits(out, 1, 1, &bit);
    // 补齐到 4 字节。后面几位是 ELD 的其他标志，取 0 即默认值；
    // 补到 4 字节是为了和参考实现给的那份（F8 E8 50 00）逐字节一致。
    while (bit < 32) {
        put_bits(out, 0, 1, &bit);
    }
    return out;
}

}  // namespace pipeline
}  // namespace adisplay
