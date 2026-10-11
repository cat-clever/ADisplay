#pragma once
// AAC-ELD 解码：镜像伴音从压缩帧到 PCM。
//
// 协议层把镜像的伴音以 AAC-ELD 压缩帧交下来（480 样本/帧、44100 Hz、立体声，
// 约 92 帧/秒）。核心负责解码、平台只负责播放 —— 与视频那侧的分工一致，也正是
// C ABI 里 AdAudioFrame 从一开始就按「交错 float32」定义的原因。
//
// 为什么用 FFmpeg 而不是各平台的解码器：AAC-ELD 的支持面很窄 —— macOS 的
// AudioToolbox 认，Windows 的 Media Foundation 不认，Android 的 MediaCodec
// 也不保证认。FFmpeg 本来就在依赖里（媒体管线用它换封装），一份实现三端通用。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace adisplay {
namespace pipeline {

class AacEldDecoder {
public:
    AacEldDecoder();
    ~AacEldDecoder();

    AacEldDecoder(const AacEldDecoder&) = delete;
    AacEldDecoder& operator=(const AacEldDecoder&) = delete;

    // 采样率与声道数就用镜像伴音那组（见 AacEldConfig.h 的常量）。
    bool init(int sample_rate, int channels, std::string* out_error);

    // 解一帧。pcm 是**交错** float32（LRLRLR…），长度 = 每声道样本数 × 声道数。
    // 返回 false 表示这一帧没解出来 —— 不致命，继续喂下一帧即可（丢一帧
    // 十几毫秒，听感上是一声极短的静音，比中断整条流好得多）。
    bool decode(const uint8_t* data, std::size_t size, std::vector<float>* pcm,
                int* out_frame_count);

    void reset();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace pipeline
}  // namespace adisplay
