#include <adisplay/pipeline/MirrorVideoDecoder.h>

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace adisplay {
namespace pipeline {

namespace {

// 解一帧最多往外吐几张图。正常是 1；带 B 帧的流解码器会缓存，多收几张能
// 保证我们留下的是最新的那一张，而不是延迟一拍的旧图。
constexpr int kMaxFramesPerPacket = 8;

// ---------------------------------------------------------------------------
// YUV → RGB 的定点系数
//
// 为什么不用 libswscale：它在 vcpkg 里是 ffmpeg 的一个独立 feature，加上它就让
// FFmpeg 的 ABI 变了 —— 而 release 是 tag 触发的，tag 上创建的缓存对其他 ref
// 不可见（只有默认分支的缓存共享），于是那份新二进制永远进不了后续轮次能读到的
// 缓存，每轮都要重编 FFmpeg（14 分钟）。为 0.9 毫秒一帧的画面付这个代价不值。
//
// 系数是四组：BT.601 / BT.709 × 有限范围（video）/ 全范围（JPEG），16 位定点。
// 选择规则刻意与 swscale 一致 —— 帧里标了 colorspace 就用标的，没标就 SD 用
// 601、HD 用 709；范围没标按有限范围。这样画面与换掉 swscale 之前完全一样。
// ---------------------------------------------------------------------------
struct YuvMatrix {
    int y_gain;
    int r_cr;
    int g_cb;
    int g_cr;
    int b_cb;
};

YuvMatrix pick_matrix(int colorspace, int range, int width) {
    const bool full_range = (range == AVCOL_RANGE_JPEG);
    bool bt709;
    switch (colorspace) {
        case AVCOL_SPC_BT709:
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            bt709 = true;
            break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            bt709 = false;
            break;
        default:
            // 未标注：高清按 709、标清按 601 —— H.264/H.265 的通行约定。
            bt709 = width > 720;
            break;
    }

    if (full_range) {
        // 全范围不需要 Y 的偏移与缩放。
        const YuvMatrix table709 = {65536, 103206, -12275, -30678, 121609};
        const YuvMatrix table601 = {65536, 91881, -22554, -46802, 116130};
        return bt709 ? table709 : table601;
    }
    // 有限范围：Y 要减 16 并按 255/219 展开，色度要减 128 并按 255/224 展开。
    const YuvMatrix table709 = {76283, 117489, -13975, -34925, 138438};
    const YuvMatrix table601 = {76283, 104595, -25675, -53279, 132203};
    return bt709 ? table709 : table601;
}

inline uint8_t clamp_to_byte(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

// 输出一行 BGRA。y_row 是亮度行，u_row / v_row 是色度行；NV12 时 v_row = u_row+1
// 且 chroma_step = 2（色度是交错的），平面格式则 chroma_step = 1。
void convert_row(const YuvMatrix& matrix, bool limited, const uint8_t* y_row,
                 const uint8_t* u_row, const uint8_t* v_row, int chroma_step,
                 uint8_t* out, int width) {
    for (int x = 0; x < width; ++x) {
        int luma = static_cast<int>(y_row[x]);
        if (limited) {
            // 刻意**不**把 (Y-16) 截到 0：低于有限范围黑电平的样点（压缩振铃会让
            // 它出现）保留负值，只在最后裁 RGB —— ffmpeg 的 swscale 就是这么做的。
            // 本机拿 swscale 当参照逐像素比对过：截断会让这类像素的绿/蓝差出 20。
            luma -= 16;
        }
        const int u = static_cast<int>(u_row[(x >> 1) * chroma_step]) - 128;
        const int v = static_cast<int>(v_row[(x >> 1) * chroma_step]) - 128;

        const int base = matrix.y_gain * luma;
        const int r = (base + matrix.r_cr * v + 32768) >> 16;
        const int g = (base + matrix.g_cb * u + matrix.g_cr * v + 32768) >> 16;
        const int b = (base + matrix.b_cb * u + 32768) >> 16;

        out[x * 4 + 0] = clamp_to_byte(b);
        out[x * 4 + 1] = clamp_to_byte(g);
        out[x * 4 + 2] = clamp_to_byte(r);
        out[x * 4 + 3] = 255;
    }
}

}  // namespace

struct MirrorVideoDecoder::Impl {
    AVCodecContext* context = nullptr;
    AVFrame* decoded = nullptr;
    AVPacket* packet = nullptr;

    // 当前解码器对应的编码。0 表示还没建。
    bool is_h265 = false;
    int codec_built = 0;

    std::string failure;

    ~Impl() {
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (decoded != nullptr) {
            av_frame_free(&decoded);
        }
        if (context != nullptr) {
            avcodec_free_context(&context);
        }
    }

    void release_codec() {
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

        const AVFrame* source = impl_->decoded;
        const int width = source->width;
        const int height = source->height;
        if (width > 0 && height > 0) {
            const bool planar420 = source->format == AV_PIX_FMT_YUV420P
                                   || source->format == AV_PIX_FMT_YUVJ420P;
            const bool semi_planar = source->format == AV_PIX_FMT_NV12;
            if (!planar420 && !semi_planar) {
                // 没见过的像素格式。当作「这一路解不了」上报，让核心退回界面层
                // 自己解那条路 —— 那条走系统播放器，通常认得出更多格式。
                const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(source->format));
                impl_->failure = std::string("解出来的像素格式不支持：")
                                 + (name != nullptr ? name : "未知");
                if (out_error != nullptr) {
                    *out_error = impl_->failure;
                }
                av_frame_unref(impl_->decoded);
                return false;
            }

            const YuvMatrix matrix =
                pick_matrix(source->colorspace, source->color_range, width);
            const bool limited = source->color_range != AVCOL_RANGE_JPEG;
            // 目标按**紧密排列**给出（行距 = width*4）：界面层要整块拷进位图缓冲，
            // 中间不留对齐空洞。
            out_bgra->resize(static_cast<std::size_t>(width) * 4 * height);
            uint8_t* destination = out_bgra->data();

            for (int row = 0; row < height; ++row) {
                const uint8_t* luma = source->data[0] + static_cast<std::ptrdiff_t>(row) * source->linesize[0];
                uint8_t* output = destination + static_cast<std::size_t>(row) * width * 4;
                if (planar420) {
                    const uint8_t* u_plane = source->data[1]
                        + static_cast<std::ptrdiff_t>(row >> 1) * source->linesize[1];
                    const uint8_t* v_plane = source->data[2]
                        + static_cast<std::ptrdiff_t>(row >> 1) * source->linesize[2];
                    convert_row(matrix, limited, luma, u_plane, v_plane, 1, output, width);
                } else {
                    // NV12：色度交错在一个平面里，U 在前、V 在后一格。
                    const uint8_t* uv_plane = source->data[1]
                        + static_cast<std::ptrdiff_t>(row >> 1) * source->linesize[1];
                    convert_row(matrix, limited, luma, uv_plane, uv_plane + 1, 2, output, width);
                }
            }

            *out_width = width;
            *out_height = height;
            got_frame = true;
        }

        av_frame_unref(impl_->decoded);
    }

    return got_frame;
}

}  // namespace pipeline
}  // namespace adisplay
