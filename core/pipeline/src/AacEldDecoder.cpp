#include <adisplay/pipeline/AacEldDecoder.h>

#include <adisplay/pipeline/AacEldConfig.h>

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

namespace adisplay {
namespace pipeline {

struct AacEldDecoder::Impl {
    AVCodecContext* context = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;

    ~Impl() {
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
        if (context != nullptr) {
            avcodec_free_context(&context);
        }
    }
};

AacEldDecoder::AacEldDecoder() : impl_(new Impl()) {}

AacEldDecoder::~AacEldDecoder() {
    delete impl_;
    impl_ = nullptr;
}

bool AacEldDecoder::init(int sample_rate, int channels, std::string* out_error) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (codec == nullptr) {
        if (out_error != nullptr) {
            *out_error = "这个 FFmpeg 构建里没有 AAC 解码器";
        }
        return false;
    }

    impl_->context = avcodec_alloc_context3(codec);
    if (impl_->context == nullptr) {
        if (out_error != nullptr) {
            *out_error = "分配解码器上下文失败";
        }
        return false;
    }

    // 解码器唯一需要的外部信息就是 AudioSpecificConfig。ELD 的数据帧里不带
    // 任何参数，缺了这几个字节它一个样本都不出、也不报错 —— 所以这一段的
    // 位序有自己的单测（见 AacEldConfig.h）。
    const std::vector<uint8_t> extradata = build_aac_eld_extradata(sample_rate, channels);
    const int padding = AV_INPUT_BUFFER_PADDING_SIZE;
    impl_->context->extradata = static_cast<uint8_t*>(
        av_mallocz(extradata.size() + static_cast<std::size_t>(padding)));
    if (impl_->context->extradata == nullptr) {
        if (out_error != nullptr) {
            *out_error = "分配解码器配置失败";
        }
        return false;
    }
    std::memcpy(impl_->context->extradata, extradata.data(), extradata.size());
    impl_->context->extradata_size = static_cast<int>(extradata.size());

    impl_->context->sample_rate = sample_rate;
    av_channel_layout_default(&impl_->context->ch_layout, channels);

    const int opened = avcodec_open2(impl_->context, codec, nullptr);
    if (opened < 0) {
        if (out_error != nullptr) {
            *out_error = "打开 AAC 解码器失败（" + std::to_string(opened) + "）";
        }
        return false;
    }

    impl_->frame = av_frame_alloc();
    impl_->packet = av_packet_alloc();
    if (impl_->frame == nullptr || impl_->packet == nullptr) {
        if (out_error != nullptr) {
            *out_error = "分配解码缓冲失败";
        }
        return false;
    }
    return true;
}

bool AacEldDecoder::decode(const uint8_t* data, std::size_t size, std::vector<float>* pcm,
                           int* out_frame_count) {
    if (impl_->context == nullptr || data == nullptr || size == 0) {
        return false;
    }
    if (out_frame_count != nullptr) {
        *out_frame_count = 0;
    }

    // 数据要先拷进 AVPacket：解码器不改它的内容，但会读它、且要求尾部有
    // AV_INPUT_BUFFER_PADDING_SIZE 的零填充，直接指外面的指针不安全。
    if (av_new_packet(impl_->packet, static_cast<int>(size)) < 0) {
        return false;
    }
    std::memcpy(impl_->packet->data, data, size);

    const int sent = avcodec_send_packet(impl_->context, impl_->packet);
    av_packet_unref(impl_->packet);
    if (sent < 0) {
        return false;
    }

    const int received = avcodec_receive_frame(impl_->context, impl_->frame);
    if (received < 0) {
        // EAGAIN 表示解码器还在攒数据（AAC-ELD 有解码器内部时延），不是错误。
        return false;
    }

    // AAC 解码器输出的是**平面** float32（FLTP），而 ABI 约定的是交错 ——
    // 所以这里手动交织。用 libswresample 也能做，但那要多链一个库，
    // 而这件事本身就是一个双层循环。
    const int channels = impl_->frame->ch_layout.nb_channels;
    const int frames = impl_->frame->nb_samples;
    if (channels <= 0 || frames <= 0 || impl_->frame->format != AV_SAMPLE_FMT_FLTP) {
        av_frame_unref(impl_->frame);
        return false;
    }

    pcm->resize(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
    for (int channel = 0; channel < channels; ++channel) {
        const float* source = reinterpret_cast<const float*>(impl_->frame->data[channel]);
        if (source == nullptr) {
            continue;
        }
        for (int i = 0; i < frames; ++i) {
            (*pcm)[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels)
                   + static_cast<std::size_t>(channel)] = source[i];
        }
    }
    if (out_frame_count != nullptr) {
        *out_frame_count = frames;
    }

    av_frame_unref(impl_->frame);
    return true;
}

void AacEldDecoder::reset() {
    if (impl_->context != nullptr) {
        avcodec_flush_buffers(impl_->context);
    }
}

}  // namespace pipeline
}  // namespace adisplay
