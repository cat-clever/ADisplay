// 这份构建没有编入 FFmpeg（Android 的核心库走平台自己的解码器），因此镜像视频
// 解码器没有实现。这里给出同一套接口的替身，让调用方不必条件编译 —— 与
// AacEldDecoderUnsupported 是同一个路子。
//
// Android 本来也走不到这里：它的 MediaCodec 自己解，核心只转发压缩帧。这个替身
// 是为了让链接成立；万一真走到了，报出来的原因是清楚的。

#include <adisplay/pipeline/MirrorVideoDecoder.h>

namespace adisplay {
namespace pipeline {

struct MirrorVideoDecoder::Impl {};

MirrorVideoDecoder::MirrorVideoDecoder() : impl_(new Impl()) {}

MirrorVideoDecoder::~MirrorVideoDecoder() {
    delete impl_;
    impl_ = nullptr;
}

bool MirrorVideoDecoder::decode(const uint8_t* data, std::size_t size, bool is_h265,
                                std::vector<uint8_t>* out_bgra,
                                int* out_width, int* out_height, std::string* out_error) {
    (void) data;
    (void) size;
    (void) is_h265;
    (void) out_bgra;
    (void) out_width;
    (void) out_height;
    if (out_error != nullptr) {
        *out_error = "这份构建没有编入 FFmpeg，镜像视频解码不可用";
    }
    return false;
}

void MirrorVideoDecoder::reset() {}

}  // namespace pipeline
}  // namespace adisplay
