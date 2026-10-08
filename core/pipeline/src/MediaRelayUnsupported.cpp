// ADisplay —— 本地换封装中转的「本平台不需要」实现
//
// 给 Android 用。它存在的意义是让调用方无需条件编译：AdEngine 那边照常构造、
// 启动、调 resolve_for_playback，只是这里把地址原样返回、什么也不做。
//
// 为什么 Android 上不编真的那份：
//
//   电视端的播放器是 ExoPlayer + MediaCodec，**本来就能直接放 HEVC-in-TS** ——
//   macOS 上 AVPlayer 拒绝 HEVC-in-TS 才是这套换封装的起因。在电视上把 TS 先
//   本地重封装成 fMP4、再交给一个能直接吃 TS 的播放器，是净亏：多一次拷贝、
//   多一份 CPU，画质还不会有任何提升。
//
//   代价也很大：FFmpeg 的 libavcodec + libavformat 静态链进来会让
//   libcastcore.so 涨到 120 MB 左右，整个 APK 从 ~30 MB 变成 128 MB。
//
// 所以 Android 既不编这部分，也不链接 FFmpeg（vcpkg.json 里给 ffmpeg 标了
// platform: !android）。将来若真遇到 ExoPlayer 吃不下的封装，再把它打开就是。
#include <adisplay/pipeline/MediaRelay.h>

#include <adisplay/common/Log.h>

namespace adisplay::pipeline {

struct MediaRelay::Impl {
    bool started = false;
};

MediaRelay::MediaRelay() : impl_(new Impl()) {}

MediaRelay::~MediaRelay() = default;

bool MediaRelay::start(std::string* out_error) {
    impl_->started = true;
    // Android 上这是正常状态而不是降级：播放器直接吃源流。
    AD_LOG_DEBUG("本平台不做本地换封装：ExoPlayer 能直接播放 HEVC-in-TS，无需中转");
    if (out_error != nullptr) {
        out_error->clear();
    }
    return true;
}

void MediaRelay::stop() {
    impl_->started = false;
}

bool MediaRelay::is_running() const {
    return impl_->started;
}

std::string MediaRelay::resolve_for_playback(const std::string& url) const {
    return url;
}

uint16_t MediaRelay::port() const {
    return 0;
}

bool MediaRelay::is_relaying() const {
    return false;
}

std::string MediaRelay::last_error() const {
    return std::string();
}

}  // namespace adisplay::pipeline
