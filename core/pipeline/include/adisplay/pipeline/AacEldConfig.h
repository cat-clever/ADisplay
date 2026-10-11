#pragma once
// AAC-ELD 的 AudioSpecificConfig。
//
// 这是 AAC 解码器唯一需要的外部信息：对象类型、采样率、声道数，外加 ELD 特有
// 的帧长标志。协议层交下来的每帧 AAC-ELD 数据本身不带这些 —— 缺了它，解码器
// 一个样本都不出。
//
// 单独成一个文件（而不是塞进解码器里）有两个理由：它是这条路上最容易写错、
// 又最难从现象反推的一处 —— 字节错了只表现为「没有声音」，而它不依赖 FFmpeg，
// 所以单测能在本机跑，拿已知的参考值钉死。

#include <cstdint>
#include <vector>

namespace adisplay {
namespace pipeline {

// 镜像伴音的实际参数：480 样本/帧、44100 Hz、立体声。
// 480 / 44100 ≈ 10.9 毫秒一帧，也就是每秒约 92 帧 —— 与真机日志里的计数一致。
constexpr int kAirplayMirrorAudioSampleRate = 44100;
constexpr int kAirplayMirrorAudioChannels = 2;

// 按 ISO/IEC 14496-3 的 AudioSpecificConfig 拼出 ELD 的配置字节。
//
// 已知参考值：44100 Hz 立体声 = F8 E8 50 00（单测拿它钉住）。
std::vector<uint8_t> build_aac_eld_extradata(int sample_rate, int channels);

}  // namespace pipeline
}  // namespace adisplay
