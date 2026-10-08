// ADisplay —— 本地换封装中转的「本平台不提供」实现
//
// 给不编 FFmpeg 那条路的平台用（当前只有 Linux）。存在的意义是让调用方
// 无需条件编译：EngineModel / DlnaBridge 那边照常构造、启动、调
// resolve_for_playback，只是这里把地址原样返回、什么也不做。
//
// 为什么 Linux 上不编：FFmpeg 的部分 x86 汇编目标文件（libavcodec.a 里的
// vc1dsp_mmx.o 等）不是位置无关代码，而 NASM 那一步拿不到 --enable-pic，
// 于是链进 libcastcore.so 时报
//
//   relocation R_X86_64_PC32 against symbol `ff_pw_9' can not be used when
//   making a shared object; recompile with -fPIC
//
// 为它去 patch 第三方 port，或者给产物加 -z,notext（允许文本重定位，能链上
// 但丢掉一部分加固），都是为不存在的需求付代价 —— 文档把 Linux 桌面端标为
// P2「暂缓」，它只在 CI 里编核心库并跑单元测试、不是交付平台。
//
// Android 不在这条路上：那边的 FFmpeg 由 Android 工具链编译，本身带 PIC，
// 而且 CMAKE_SYSTEM_NAME 是 "Android" 而不是 "Linux"。
#include <adisplay/pipeline/MediaRelay.h>

#include <adisplay/common/Log.h>

namespace adisplay::pipeline {

struct MediaRelay::Impl {
    bool started = false;
    std::string explanation;
};

MediaRelay::MediaRelay() : impl_(new Impl()) {}

MediaRelay::~MediaRelay() = default;

bool MediaRelay::start(std::string* out_error) {
    impl_->started = true;
    impl_->explanation =
        "本平台不提供本地换封装中转（FFmpeg 的 x86 汇编目标文件不是位置无关代码，"
        "链不进共享库；Linux 桌面端按文档属于 P2「暂缓」，只用于编译验证与单测）。"
        "HEVC-in-TS 的片源在本平台不会自动换封装。";

    // 只打一次日志说明情况。返回 true 而不是失败 —— 本平台就是不做这件事，
    // 报成失败会让上层每轮都记一条没有意义的警告。
    AD_LOG_INFO("{}", impl_->explanation);

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
    return impl_->explanation;
}

}  // namespace adisplay::pipeline
