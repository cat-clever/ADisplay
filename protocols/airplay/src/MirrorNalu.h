#pragma once
// AirPlay 镜像流的 NALU 规范化。
//
// 协议层（UxPlay）交上来的镜像帧有两个性质，都必须在越过 C ABI 之前收拾干净：
//
//   1. 它是 **Annex B** —— 每个 NALU 前面是 00 00 01 / 00 00 00 01 起始码。
//      这是从上游源码确认的：raop_rtp_mirror.c 把入包里每个 NALU 的长度前缀
//      逐个换成了起始码。平台解码器要的是 AVCC，那一步转换由各平台自己做。
//
//   2. 参数集（SPS/PPS）**只挂在第一帧上**。上游拼完一次就把标志清掉，此后
//      只有切分辨率、暂停恢复才会再带一次。这对界面层是个陷阱：帧回调与
//      会话回调不在同一个线程上，界面要等主线程转一圈才能把渲染面建出来并挂上，
//      而第一帧在同一毫秒内就到了 —— 它被丢在「还没有落点」的时候，参数集
//      从此再也不来，解码配置永远建不出来。表现是「核心这边帧一直在计数，
//      屏幕上始终全黑」，而日志里看不出任何异常。
//
// 所以这里多做一件事：把参数集缓存下来，之后每个关键帧都给它补上。这样渲染面
// 无论什么时候挂上来，一两个关键帧之内就能出画面（也顺带覆盖了切换全屏、
// 渲染面重建这些会重新挂载的情况）。帧自带的参数集以帧为准，不会补成两份。

#include <cstddef>
#include <cstdint>
#include <vector>

namespace adisplay {
namespace airplay {

using NaluUnit = std::vector<uint8_t>;

// 按起始码切开一帧。返回的单元不含起始码，也不含单元之间多余的填充零。
std::vector<NaluUnit> split_annex_b(const uint8_t* data, std::size_t size);

// 挑出参数集：H.264 是 SPS(7) / PPS(8)，H.265 是 VPS(32) / SPS(33) / PPS(34)。
// 只有凑齐一整套时才返回（H.264 要两个，H.265 要三个）—— 残缺的一套建不出
// 解码配置，当成没有更安全。
std::vector<NaluUnit> select_parameter_sets(const std::vector<NaluUnit>& units, bool is_h265);

// 帧内是否含关键帧（H.264 的 IDR 是 5；H.265 的 IDR 是 19/20）。
bool contains_keyframe(const std::vector<NaluUnit>& units, bool is_h265);

// 按 Annex B 拼回去（每个单元前加 00 00 00 01）。
std::vector<uint8_t> join_annex_b(const std::vector<NaluUnit>& units);

// 一个镜像会话一个规范器。它会记住参数集，给关键帧补齐。
class MirrorNaluNormalizer {
public:
    // 整理一帧，返回可以直接交给平台解码器的 Annex B 字节。
    // 返回空表示这一帧没有可用的内容（空包、或一个 NALU 都没切出来）。
    std::vector<uint8_t> normalize(const uint8_t* data, std::size_t size, bool is_h265);

    // 复位（会话重开、编码器切换时调用）。
    void reset();

    // 已缓存的参数集个数。仅供日志与测试观察。
    std::size_t cached_parameter_set_count() const { return parameter_sets_.size(); }

    // 这一帧是否被补过参数集。仅供日志观察。
    bool last_frame_had_prepended_sets() const { return last_prepended_; }

private:
    bool is_h265_ = false;
    bool codec_known_ = false;
    bool last_prepended_ = false;
    std::vector<NaluUnit> parameter_sets_;
};

}  // namespace airplay
}  // namespace adisplay
