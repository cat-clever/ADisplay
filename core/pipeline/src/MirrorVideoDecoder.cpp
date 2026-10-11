#include <adisplay/pipeline/MirrorVideoDecoder.h>

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace adisplay {
namespace pipeline {

namespace {

// 解一帧最多往外吐几张图。正常是 1；带 B 帧的流解码器会缓存，多收几张能
// 保证我们留下的是最新的那一张，而不是延迟一拍的旧图。
constexpr int kMaxFramesPerPacket = 8;

}  // namespace

struct MirrorVideoDecoder::Impl {
    AVCodecContext* context = nullptr;
    AVFrame* decoded = nullptr;    // 解码器输出
    AVFrame* bgra = nullptr;       // 转换目标
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;

    int sws_width = 0;
    int sws_height = 0;
    int sws_src_format = -1;

    // 当前解码器对应的编码。0 表示还没建。
    bool is_h265 = false;
    int codec_built = 0;

    std::string failure;

    ~Impl() {
        if (sws != nullptr) {
            sws_freeContext(sws);
        }
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (bgra != nullptr) {
            av_frame_free(&bgra);
        }
        if (decoded != nullptr) {
            av_frame_free(&decoded);
        }
        if (context != nullptr) {
            avcodec_free_context(&context);
        }
    }

    void release_codec() {
        if (sws != nullptr) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        sws_width = 0;
        sws_height = 0;
        sws_src_format = -1;
        if (context != nullptr) {
            avcodec_free_context(&context);
        }
        codec_built = 0;
    }
};

MirrorVideoDecoder::MirrorVideoDecoder() : impl_(new Impl()) {}

MirrorVideoDecoder::~MirrorVideoDecoder() {
    delete impl_;
    impl_ = nullptr;
}

void MirrorVideoDecoder::reset() {
    impl_->release_codec();
    impl_->failure.clear();
    if (impl_->decoded != nullptr) {
        av_frame_unref(impl_->decoded);
    }
    if (impl_->bgra != nullptr) {
        av_frame_unref(impl_->bgra);
    }
}

bool MirrorVideoDecoder::decode(const uint8_t* data, std::size_t size, bool is_h265,
                                std::vector<uint8_t>* out_bgra,
                                int* out_width, int* out_height, std::string* out_error) {
    if (data == nullptr || size == 0 || out_bgra == nullptr) {
        return false;
    }
    if (!impl_->failure.empty()) {
        // 已经判定起不来了：把原因原样再报一次，让调用方有机会记进日志。
        if (out_error != nullptr) {
            *out_error = impl_->failure;
        }
        return false;
    }

    if (impl_->codec_built == 0 || impl_->is_h265 != is_h265) {
        // 解码器按编码各建一次；H.264 与 H.265 之间切换时整套重建。
        impl_->release_codec();

        std::string error;
        const AVCodec* codec =
            avcodec_find_decoder(is_h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
        if (codec == nullptr) {
            error = is_h265 ? "这个 FFmpeg 构建里没有 H.265 解码器"
                            : "这个 FFmpeg 构建里没有 H.264 解码器";
        } else {
            impl_->context = avcodec_alloc_context3(codec);
            if (impl_->context == nullptr) {
                error = "分配解码器上下文失败";
            } else {
                // 单线程解码，刻意为之。
                //
                // FFmpeg 默认的帧级多线程要先缓存 thread_count 帧才往外吐（它靠
                // 这个做帧间并行），延迟就是「线程数 ÷ 帧率」—— 十几核的机器上
                // 半秒上下。而镜像这边一帧只要 0.9 毫秒，离 33 毫秒的帧间隔差得远，
                // 多线程什么也换不来，只换来延迟。
                impl_->context->thread_count = 1;
                if (avcodec_open2(impl_->context, codec, nullptr) < 0) {
                    error = "打开解码器失败";
                } else {
                    impl_->codec_built = 1;
                    impl_->is_h265 = is_h265;
                }
            }
        }

        if (!error.empty()) {
            impl_->failure = error;
            if (out_error != nullptr) {
                *out_error = error;
            }
            return false;
        }
    }

    if (impl_->packet == nullptr) {
        impl_->packet = av_packet_alloc();
        if (impl_->packet == nullptr) {
            impl_->failure = "分配 AVPacket 失败";
            return false;
        }
    }
    if (impl_->decoded == nullptr) {
        impl_->decoded = av_frame_alloc();
        if (impl_->decoded == nullptr) {
            impl_->failure = "分配 AVFrame 失败";
            return false;
        }
    }

    // 用 av_new_packet 而不是直接指外面的缓冲：FFmpeg 的解码器允许在数据末尾
    // 多读几个字节（AV_INPUT_BUFFER_PADDING_SIZE），av_new_packet 会补上那段
    // 零，直接指过去就是越界读。
    if (av_new_packet(impl_->packet, static_cast<int>(size)) < 0) {
        return false;
    }
    std::memcpy(impl_->packet->data, data, size);

    const int sent = avcodec_send_packet(impl_->context, impl_->packet);
    av_packet_unref(impl_->packet);
    if (sent < 0 && sent != AVERROR(EAGAIN)) {
        return false;
    }

    bool got_frame = false;
    for (int i = 0; i < kMaxFramesPerPacket; ++i) {
        const int received = avcodec_receive_frame(impl_->context, impl_->decoded);
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
            // 这一帧还没凑够，等后面的数据。
            break;
        }
        if (received < 0) {
            break;
        }

        AVFrame* source = impl_->decoded;
        const int width = source->width;
        const int height = source->height;
        if (width > 0 && height > 0) {
            if (impl_->bgra == nullptr) {
                impl_->bgra = av_frame_alloc();
            }
            if (impl_->bgra != nullptr) {
                if (impl_->sws == nullptr || impl_->sws_width != width
                    || impl_->sws_height != height
                    || impl_->sws_src_format != static_cast<int>(source->format)) {
                    if (impl_->sws != nullptr) {
                        sws_freeContext(impl_->sws);
                    }
                    // 同尺寸、只做格式与色彩空间转换，所以用最省的点采样。
                    // 色度矩阵与量化范围由 sws_scale_frame 从帧自带的
                    // colorspace / color_range 里取，不用我们猜。
                    impl_->sws = sws_getContext(width, height,
                                                static_cast<AVPixelFormat>(source->format),
                                                width, height, AV_PIX_FMT_BGRA,
                                                SWS_POINT, nullptr, nullptr, nullptr);
                    impl_->sws_width = width;
                    impl_->sws_height = height;
                    impl_->sws_src_format = static_cast<int>(source->format);
                }

                if (impl_->sws != nullptr) {
                    av_frame_unref(impl_->bgra);
                    impl_->bgra->format = AV_PIX_FMT_BGRA;
                    impl_->bgra->width = width;
                    impl_->bgra->height = height;
                    if (av_frame_get_buffer(impl_->bgra, 0) >= 0
                        && sws_scale_frame(impl_->sws, impl_->bgra, source) >= 0) {
                        // 目标按**紧密排列**给出（行距 = width*4）：界面层要整块
                        // 拷进位图缓冲，中间不留对齐空洞。
                        const int stride = impl_->bgra->linesize[0];
                        out_bgra->resize(static_cast<std::size_t>(width) * 4 * height);
                        for (int row = 0; row < height; ++row) {
                            std::memcpy(out_bgra->data() + static_cast<std::size_t>(row) * width * 4,
                                        impl_->bgra->data[0] + static_cast<std::ptrdiff_t>(row) * stride,
                                        static_cast<std::size_t>(width) * 4);
                        }
                        *out_width = width;
                        *out_height = height;
                        got_frame = true;
                    }
                }
            }
        }

        av_frame_unref(impl_->decoded);
    }

    return got_frame;
}

}  // namespace pipeline
}  // namespace adisplay
