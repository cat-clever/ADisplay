// 这份构建没有编入 FFmpeg（Android 的核心库走平台自己的解码器），因此 AAC-ELD
// 解码器没有实现。这里给出同一套接口的替身，让调用方不必条件编译 —— 与
// MediaRelay 的「本平台不需要」实现是同一个路子。
//
// 屏幕镜像的伴音在 Android 上本来也走不到这里（AirPlay 协议层压根不在那份构建
// 里）。这个替身是为了让链接成立；万一真走到了，报出来的原因是清楚的。

#include <adisplay/pipeline/AacEldDecoder.h>

namespace adisplay {
namespace pipeline {

struct AacEldDecoder::Impl {};

AacEldDecoder::AacEldDecoder() : impl_(new Impl()) {}

AacEldDecoder::~AacEldDecoder() {
    delete impl_;
    impl_ = nullptr;
}

bool AacEldDecoder::init(int sample_rate, int channels, std::string* out_error) {
    (void) sample_rate;
    (void) channels;
    if (out_error != nullptr) {
        *out_error = "这份构建没有编入 FFmpeg，AAC-ELD 解码不可用";
    }
    return false;
}

bool AacEldDecoder::decode(const uint8_t* data, std::size_t size, std::vector<float>* pcm,
                           int* out_frame_count) {
    (void) data;
    (void) size;
    (void) pcm;
    if (out_frame_count != nullptr) {
        *out_frame_count = 0;
    }
    return false;
}

void AacEldDecoder::reset() {}

}  // namespace pipeline
}  // namespace adisplay
