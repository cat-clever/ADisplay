#pragma once
// 镜像视频解码：H.264 / H.265 的 Annex B 压缩帧 → BGRA。
//
// 为什么这一路的解码放在核心，而不是像 DLNA 那样交给系统播放器：
//
// 镜像是**实时**的，要的是「最新一帧立刻上屏」。把它交给 Media Foundation
// 走流式播放时，它会先攒一段再开播（实测 12.6 秒），而且每次重新缓冲都把
// 这段延迟再叠一次 —— 三分钟能累积到 27 秒。自己解码就能只画最新那一帧，
// 来晚的直接丢，延迟是一帧。
//
// macOS 与 Android 本来就是这么做的（VideoToolbox / MediaCodec + 直接上屏），
// Windows 之前是唯一的例外 —— 也正好是唯一有延迟问题的平台。
//
// 为什么用 FFmpeg：它本来就在依赖里（管线用它换封装与解 AAC-ELD），一份实现
// 三端通用，不必再为 Windows 单独接一套 Media Foundation 解码器。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace adisplay {
namespace pipeline {

class MirrorVideoDecoder {
public:
    MirrorVideoDecoder();
    ~MirrorVideoDecoder();

    MirrorVideoDecoder(const MirrorVideoDecoder&) = delete;
    MirrorVideoDecoder& operator=(const MirrorVideoDecoder&) = delete;

    // 解一帧 Annex B。解码器按需要在 H.264 与 H.265 之间自己切换。
    //
    // 返回 true 表示 out_bgra 里是**这一批里最新**的一帧（宽 = out_width，
    // 行距 = out_width * 4，字节序 BGRA）。返回 false 表示这一帧还没凑出图来
    // （参数集未到齐、或者就是解不出来）—— 丢一帧不致命，继续喂下一帧即可。
    //
    // 解码器起不来时把原因写进 out_error 并返回 false，之后不再尝试。
    bool decode(const uint8_t* data, std::size_t size, bool is_h265,
                std::vector<uint8_t>* out_bgra,
                int* out_width, int* out_height, std::string* out_error);

    // 会话重开时调用：丢掉解码器与中间状态。
    void reset();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace pipeline
}  // namespace adisplay
