#include "Mp4Remuxer.h"

#include <adisplay/common/Log.h>

// FFmpeg 的头文件是 C 的，自身不带 extern "C" 包裹（libavutil 里那处
// __cplusplus 是别的东西），所以必须自己包上，否则链接期找不到被 C++ 名字
// 改编过的符号。
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <cstring>
#include <limits>

namespace adisplay::pipeline {
namespace {

// AVIO 的缓冲区大小。TS 分片普遍是几百 KB，32KB 一进一出足够。
constexpr int kIoBufferSize = 32768;

// FFmpeg 的错误文案。数值也一并带上，因为有几个码的文案会把人带偏：
// 裸的 -1 被 av_strerror 翻成 "Operation not permitted"，看着像文件权限问题，
// 实际上大量地方用 -1 表示「通用失败」（比如 movenc 拒绝 ADTS 格式的 AAC）。
// 没有数值时，看到那句话会往完全错误的方向查。
std::string av_error_text(int error) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(error, buffer, sizeof(buffer));
    return std::string(buffer) + "（错误码 " + std::to_string(error) + "）";
}

int write_callback(void* opaque, const uint8_t* data, int size) {
    auto* sink = static_cast<OutputSink*>(opaque);
    if (sink == nullptr || sink->bytes == nullptr || size <= 0) {
        return 0;
    }
    const std::size_t end = sink->pos + static_cast<std::size_t>(size);
    if (sink->bytes->size() < end) {
        sink->bytes->resize(end);
    }
    std::memcpy(sink->bytes->data() + sink->pos, data, static_cast<std::size_t>(size));
    sink->pos = end;
    return size;
}

// 输出侧的定位。内存流的定位本来就没有代价，给它一个真实现是有原因的：
// MOV 复用器在写分片时会去 seek，而 FFmpeg 对「不可寻址的输出」上的 seek
// 返回的正是 AVERROR(EPERM) —— 文案是 "Operation not permitted"，
// 看起来像文件权限问题，实际与权限毫无关系。
int64_t seek_callback(void* opaque, int64_t offset, int whence) {
    auto* sink = static_cast<OutputSink*>(opaque);
    if (sink == nullptr || sink->bytes == nullptr) {
        return AVERROR(EINVAL);
    }

    if ((whence & AVSEEK_SIZE) != 0) {
        // 复用器问「你有多大」，直接给答案，不要动位置。
        return static_cast<int64_t>(sink->bytes->size());
    }

    const int mode = whence & ~AVSEEK_FORCE;
    const int64_t size = static_cast<int64_t>(sink->bytes->size());
    int64_t target = 0;
    if (mode == SEEK_SET) {
        target = offset;
    } else if (mode == SEEK_CUR) {
        target = static_cast<int64_t>(sink->pos) + offset;
    } else if (mode == SEEK_END) {
        target = size + offset;
    } else {
        return AVERROR(EINVAL);
    }

    if (target < 0) {
        return AVERROR(EINVAL);
    }
    // 往前跳（回填之后又跳回来）不能缩短已有内容；往后退则补零，
    // 随后写入的字节会覆盖它。
    if (target > size) {
        sink->bytes->resize(static_cast<std::size_t>(target), 0);
    }
    sink->pos = static_cast<std::size_t>(target);
    return target;
}

// 输入侧的只读内存流。
struct MemoryReader {
    const uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
};

int read_callback(void* opaque, uint8_t* buffer, int size) {
    auto* reader = static_cast<MemoryReader*>(opaque);
    if (reader == nullptr || size <= 0) {
        return AVERROR(EINVAL);
    }
    if (reader->pos >= reader->size) {
        // 流协议的回调不能返回 0，必须给出明确的结束码，否则解复用器会一直等。
        return AVERROR_EOF;
    }
    const std::size_t remaining = reader->size - reader->pos;
    const std::size_t wanted = static_cast<std::size_t>(size);
    const std::size_t taken = remaining < wanted ? remaining : wanted;
    std::memcpy(buffer, reader->data + reader->pos, taken);
    reader->pos += taken;
    return static_cast<int>(taken);
}

// 预读上限。正常情况下头两个包之内就能见到音频，给足余量只是为了避免
// 极端素材（例如开头几十秒只有视频）把整个分片读完。
constexpr int kPrimePacketLimit = 400;

// 放掉预读攒下的包。它们在处理时已被逐个搬空，这里只回收对象本身。
void release_pending(std::vector<AVPacket*>* pending) {
    for (AVPacket*& pkt : *pending) {
        if (pkt != nullptr) {
            av_packet_free(&pkt);
        }
    }
    pending->clear();
}

// 释放一次分片解析用的输入。自定义 AVIO 由我们自己释放（见 ensure_output 里
// 关于 pb 归属的说明）。
void release_input(AVFormatContext** input, AVIOContext** io) {
    if (*input != nullptr) {
        (*input)->pb = nullptr;
        avformat_close_input(input);
    }
    if (*io != nullptr) {
        avio_context_free(io);
    }
}

}  // namespace

Mp4Remuxer::Mp4Remuxer() = default;

Mp4Remuxer::~Mp4Remuxer() {
    close_output();
}

void Mp4Remuxer::close_output() {
    release_pending(&pending_packets_);
    aac_config_.clear();

    for (AVBSFContext*& ctx : bsf_) {
        if (ctx != nullptr) {
            av_bsf_free(&ctx);
        }
    }
    bsf_.clear();

    if (output_ != nullptr) {
        if (header_written_) {
            // 收尾只是把最后一段刷出来落地，失败也没什么可做的。没写过头的
            // 上下文不能调 trailer —— 复用器内部还没有任何轨。
            av_write_trailer(output_);
            header_written_ = false;
        }
        // 输出用的 AVIO 是我们自己的：置空 pb 再释放上下文，避开「某些版本
        // 会顺手把 pb 一起释放」的差异，免得重复释放。
        output_->pb = nullptr;
        avformat_free_context(output_);
        output_ = nullptr;
    }
    if (output_io_ != nullptr) {
        avio_context_free(&output_io_);
    }
}

void Mp4Remuxer::prime_aac_config(AVFormatContext* input) {
    aac_config_.assign(input->nb_streams, std::vector<uint8_t>());

    // 找出需要预读的流。只有 AAC 需要：视频的参数集（SPS/PPS）由 movenc 从
    // 码流里自己提取，不需要我们在写头之前准备什么。
    std::vector<unsigned int> need_config;
    for (unsigned int i = 0; i < input->nb_streams; ++i) {
        if (input->streams[i]->codecpar->codec_id == AV_CODEC_ID_AAC) {
            need_config.push_back(i);
        }
    }
    if (need_config.empty()) {
        return;
    }

    const AVBitStreamFilter* filter = av_bsf_get_by_name("aac_adtstoasc");
    if (filter == nullptr) {
        return;   // 没有这个过滤器，后面也走不通，交给调用方报错
    }

    for (unsigned int index : need_config) {
        // 这一份过滤器只用来取配置，用完即弃 —— 真正转码流的那一份在
        // ensure_output 里另建，两者不共用，免得互相吃掉对方的输入。
        AVBSFContext* ctx = nullptr;
        if (av_bsf_alloc(filter, &ctx) < 0 || ctx == nullptr) {
            continue;
        }
        if (avcodec_parameters_copy(ctx->par_in, input->streams[index]->codecpar) < 0 ||
            av_bsf_init(ctx) < 0) {
            av_bsf_free(&ctx);
            continue;
        }

        bool produced = false;
        for (int guard = 0; !produced && guard < kPrimePacketLimit; ++guard) {
            AVPacket* pkt = av_packet_alloc();
            if (pkt == nullptr || av_read_frame(input, pkt) < 0) {
                if (pkt != nullptr) {
                    av_packet_free(&pkt);
                }
                break;   // 读到末尾还没见到音频
            }
            // 不管是不是音频都留着：它是这个分片开头的一帧，不能丢。
            pending_packets_.push_back(pkt);

            if (static_cast<unsigned int>(pkt->stream_index) != index) {
                continue;
            }

            // 送一份拷贝给过滤器：send 会把包搬空，而原包还要留给后面的
            // 正常处理，不能被吃掉。
            AVPacket* clone = av_packet_clone(pkt);
            if (clone == nullptr) {
                continue;
            }
            const int send_ret = av_bsf_send_packet(ctx, clone);
            av_packet_free(&clone);   // 无论成败，这个包对象都要还回去
            if (send_ret < 0) {
                continue;
            }

            AVPacket* produced_packet = av_packet_alloc();
            if (produced_packet == nullptr) {
                continue;
            }
            if (av_bsf_receive_packet(ctx, produced_packet) >= 0) {
                produced = true;
                // 配置**不**在过滤器的 par_out 上，而是作为附属数据挂在第一条
                // 输出包上（AV_PKT_DATA_NEW_EXTRADATA）—— 这一点是从过滤器的
                // 源码里确认的：它用 av_packet_new_side_data 挂上去，而不是写
                // par_out。按 par_out 取会一直取到空。
                size_t side_size = 0;
                const uint8_t* side = av_packet_get_side_data(
                    produced_packet, AV_PKT_DATA_NEW_EXTRADATA, &side_size);
                if (side != nullptr && side_size > 0) {
                    aac_config_[index].assign(side, side + side_size);
                    AD_LOG_DEBUG("取到 AAC 解码配置，{} 字节", side_size);
                }
            }
            av_packet_free(&produced_packet);
        }

        av_bsf_free(&ctx);
    }
}

bool Mp4Remuxer::ensure_output(const AVFormatContext* input, std::string* error) {
    if (output_ != nullptr) {
        return true;
    }

    int ret = avformat_alloc_output_context2(&output_, nullptr, "mp4", nullptr);
    if (ret < 0 || output_ == nullptr) {
        output_ = nullptr;
        *error = "创建 mp4 复用器失败：" + av_error_text(ret);
        return false;
    }

    stream_map_.assign(input->nb_streams, -1);
    for (unsigned int i = 0; i < input->nb_streams; ++i) {
        const AVStream* in_stream = input->streams[i];
        const AVMediaType type = in_stream->codecpar->codec_type;
        if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) {
            // TS 里常夹带 ID3 / EPG 这类数据流，mp4 没有对应的 sample entry；
            // 字幕则要在 mp4 里换成 mov_text，那需要 bitstream filter 甚至重编码
            // —— 本模块只做直通，所以这类流一律丢掉（投屏场景也用不到，
            // 字幕一般是烧在画面里的）。
            AD_LOG_DEBUG("换封装丢弃非音视频流：类型 {}", static_cast<int>(type));
            continue;
        }
        // 编码能不能装进 mp4 不在这里预判：movenc 写头时自己会用内部标签表
        // 判定，装不下就直接报「Could not find tag for codec ...」，我们照它的
        // 报错走即可。用 avformat_query_codec 预判反而可能误伤 —— 没挂标签表
        // 的复用器在那个函数里会一律回 0。
        AVStream* out_stream = avformat_new_stream(output_, nullptr);
        if (out_stream == nullptr) {
            *error = "创建输出流失败";
            return false;
        }
        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) {
            *error = "复制编码参数失败：" + av_error_text(ret);
            return false;
        }
        // TS 里的 codec_tag 是流类型编码，不是 mp4 认得的 fourcc：不清掉的话
        // 复用器要么直接拒绝，要么写出错误的 sample entry。
        out_stream->codecpar->codec_tag = 0;
        // 时基照抄输入，movenc 会据此给这条轨挑 timescale。
        out_stream->time_base = in_stream->time_base;
        out_stream->avg_frame_rate = in_stream->avg_frame_rate;
        out_stream->sample_aspect_ratio = in_stream->sample_aspect_ratio;
        if (in_stream->codecpar->codec_id == AV_CODEC_ID_AAC) {
            // AAC 在 MP4 里需要一份 AudioSpecificConfig（esds 里的那点内容）。
            // 它藏在 ADTS 头里，要等第一条音频包被解析出来才拿得到 ——
            // 所以由 prime_aac_config 预读得到，在这里补上。
            //
            // 不能指望 find_stream_info 顺手填好：实测它对这种 TS 分片没有填，
            // 于是 esds 残缺，播放器直接拒绝打开整个文件（表现为「Cannot Open」），
            // 而不只是没有声音。
            const std::vector<uint8_t> config =
                i < aac_config_.size() ? aac_config_[i] : std::vector<uint8_t>();
            if (!config.empty()) {
                av_freep(&out_stream->codecpar->extradata);
                out_stream->codecpar->extradata = static_cast<uint8_t*>(
                    av_mallocz(config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
                if (out_stream->codecpar->extradata != nullptr) {
                    std::memcpy(out_stream->codecpar->extradata, config.data(), config.size());
                    out_stream->codecpar->extradata_size = static_cast<int>(config.size());
                }
            }
            if (out_stream->codecpar->extradata == nullptr ||
                out_stream->codecpar->extradata_size == 0) {
                // 预读也没拿到。写出来的 esds 会是残缺的，播放器多半打不开 ——
                // 这条日志是那种情况下唯一的线索。
                AD_LOG_WARN("AAC 音轨没有取到解码配置，写出的 fMP4 播放器可能打不开");
            }
        }
        stream_map_[i] = static_cast<int>(output_->nb_streams) - 1;
    }

    if (output_->nb_streams == 0) {
        *error = "分片里没有可用的音视频流";
        return false;
    }

    // AAC 要过一道 aac_adtstoasc。
    //
    // TS 里的 AAC 是 ADTS 封装（每帧前面带 0xFFF 同步头和采样率等字段），
    // 而 MP4 要的是裸 AAC 帧加一份独立的 AudioSpecificConfig。mp4 复用器遇到
    // ADTS 帧会直接拒绝，报的是
    //   "Malformed AAC bitstream detected: use the audio bitstream filter
    //    'aac_adtstoasc' to fix it"
    // 而且它的返回码是裸的 -1，av_strerror 把它翻成 "Operation not permitted" ——
    // 文案与权限毫无关系，我上一轮就是被这句话带偏去改了 seek。
    //
    // 视频那条路不需要过滤器：H.264/H.265 的起始码与长度前缀由复用器自己处理。
    bsf_.assign(output_->nb_streams, nullptr);
    for (unsigned int i = 0; i < input->nb_streams; ++i) {
        const int out_index = stream_map_[i];
        if (out_index < 0) {
            continue;
        }
        if (input->streams[i]->codecpar->codec_id != AV_CODEC_ID_AAC) {
            continue;
        }

        const AVBitStreamFilter* filter = av_bsf_get_by_name("aac_adtstoasc");
        if (filter == nullptr) {
            AD_LOG_WARN("这份 FFmpeg 没有 aac_adtstoasc 过滤器，AAC 音轨写不进 fMP4");
            continue;
        }
        AVBSFContext* ctx = nullptr;
        if (av_bsf_alloc(filter, &ctx) < 0 || ctx == nullptr) {
            AD_LOG_WARN("分配 aac_adtstoasc 过滤器失败，AAC 音轨写不进 fMP4");
            continue;
        }
        if (avcodec_parameters_copy(ctx->par_in, input->streams[i]->codecpar) < 0) {
            AD_LOG_WARN("复制 AAC 编码参数失败，AAC 音轨写不进 fMP4");
            av_bsf_free(&ctx);
            continue;
        }
        ctx->time_base_in = input->streams[i]->time_base;
        if (av_bsf_init(ctx) < 0) {
            AD_LOG_WARN("初始化 aac_adtstoasc 过滤器失败，AAC 音轨写不进 fMP4");
            av_bsf_free(&ctx);
            continue;
        }
        bsf_[static_cast<std::size_t>(out_index)] = ctx;
    }

    unsigned char* io_buffer = static_cast<unsigned char*>(av_malloc(kIoBufferSize));
    if (io_buffer == nullptr) {
        *error = "内存不足（输出缓冲）";
        return false;
    }
    sink_.bytes = &buffer_;
    sink_.pos = 0;
    output_io_ = avio_alloc_context(io_buffer, kIoBufferSize, 1, &sink_, nullptr,
                                    write_callback, seek_callback);
    if (output_io_ == nullptr) {
        av_free(io_buffer);
        *error = "创建输出 AVIO 失败";
        return false;
    }
    // 内存输出不可寻址。fMP4 这条路本来也不需要回填（moov 是空的，样本全在
    // moof 里），显式置 0 免得复用器去做它做不到的 seek。
    // 必须声明可寻址：MOV 复用器会 seek，而「不可寻址 + seek」在 FFmpeg 里
    // 返回的是 AVERROR(EPERM)（文案 "Operation not permitted"，与权限无关）。
    output_io_->seekable = AVIO_SEEKABLE_NORMAL;
    output_->pb = output_io_;
    output_->flags |= AVFMT_FLAG_CUSTOM_IO;

    AVDictionary* options = nullptr;
    // empty_moov       —— moov 只写一次且不含样本，样本都在 moof+mdat 里。
    //                     这正是能把 init 段单独切出来的前提：写头之后、
    //                     第一个 moof 之前的那段字节就是 init 段。
    // frag_keyframe    —— 开碎片化，非可寻址输出必须走这条。
    // default_base_moof + omit_tfhd_offset —— CMAF / HLS 要求的形态，播放器
    //                     按 EXT-X-MAP 拿到 init 段后能直接对上分片。
    av_dict_set(&options, "movflags",
                "frag_keyframe+empty_moov+default_base_moof+omit_tfhd_offset", 0);
    ret = avformat_write_header(output_, &options);
    av_dict_free(&options);
    if (ret < 0) {
        *error = "写 mp4 头失败：" + av_error_text(ret);
        return false;
    }

    header_written_ = true;
    // 写回调是带缓冲的，flush 之后 buffer_ 里才是完整的 ftyp+moov。
    avio_flush(output_io_);
    init_segment_.assign(buffer_.begin(), buffer_.end());
    buffer_.clear();
    sink_.pos = 0;
    has_init_ = true;

    offsets_.assign(output_->nb_streams, 0);
    timeline_next_dts_.assign(output_->nb_streams, 0);
    last_written_dts_.assign(output_->nb_streams, std::numeric_limits<int64_t>::min());

    AD_LOG_INFO("fMP4 复用器就绪：{} 条流，init 段 {} 字节", output_->nb_streams,
                init_segment_.size());
    return true;
}

bool Mp4Remuxer::append_ts_segment(const uint8_t* data, std::size_t size,
                                   std::vector<uint8_t>* out, std::string* error) {
    out->clear();
    if (data == nullptr || size == 0) {
        *error = "分片是空的";
        return false;
    }

    MemoryReader reader;
    reader.data = data;
    reader.size = size;

    unsigned char* io_buffer = static_cast<unsigned char*>(av_malloc(kIoBufferSize));
    if (io_buffer == nullptr) {
        *error = "内存不足（输入缓冲）";
        return false;
    }
    AVIOContext* input_io = avio_alloc_context(io_buffer, kIoBufferSize, 0, &reader,
                                               read_callback, nullptr, nullptr);
    if (input_io == nullptr) {
        av_free(io_buffer);
        *error = "创建输入 AVIO 失败";
        return false;
    }

    AVFormatContext* input = avformat_alloc_context();
    if (input == nullptr) {
        avio_context_free(&input_io);
        *error = "内存不足（输入上下文）";
        return false;
    }
    input->pb = input_io;
    input->flags |= AVFMT_FLAG_CUSTOM_IO;

    // 明确按 MPEG-TS 解：分片很短，靠探测容易认错，而我们本来就知道它是 TS。
    const AVInputFormat* ts_format = av_find_input_format("mpegts");
    int ret = avformat_open_input(&input, nullptr, ts_format, nullptr);
    if (ret < 0) {
        // 打开失败时 avformat_open_input 已经把 input 释放并置空，但自定义的
        // AVIO 是我们自己的，要自己还回去。
        avio_context_free(&input_io);
        *error = "打开 TS 分片失败：" + av_error_text(ret);
        return false;
    }

    ret = avformat_find_stream_info(input, nullptr);
    if (ret < 0 || input->nb_streams == 0) {
        release_input(&input, &input_io);
        *error = "分片里读不出码流信息：" + av_error_text(ret < 0 ? ret : AVERROR_INVALIDDATA);
        return false;
    }

    // 先预读、拿到 AAC 解码配置，再写头 —— 顺序不能反（见 prime_aac_config）。
    prime_aac_config(input);

    if (!ensure_output(input, error)) {
        release_input(&input, &input_io);
        close_output();
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
        release_input(&input, &input_io);
        *error = "内存不足（包）";
        return false;
    }

    const std::size_t before = buffer_.size();
    std::vector<bool> first_packet(stream_map_.size(), true);
    // 预读阶段读过的包排在所有新读的包之前，必须按这个顺序先处理 ——
    // 顺序错了音视频的交错就乱了。
    std::size_t pending_index = 0;

    // 一个包走完「时间戳整平 → 交给复用器」，返回 FFmpeg 错误码（>=0 为成功）。
    //
    // 抽成 lambda 是因为音频包要先过码流过滤器：过滤前是读进来的那个包，
    // 过滤后是过滤器吐出来的包，但两者的后续处理完全一样，不该写两遍。
    auto emit_packet = [&](AVPacket* pkt, std::size_t index,
                           unsigned int input_index) -> int {
        const int64_t dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        const int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : dts;
        if (dts == AV_NOPTS_VALUE) {
            return 0;   // 丢掉：没有时间戳的包只会让复用器报错
        }
        if (first_packet[index]) {
            // 每份分片的时间戳起点都不一样（有的从 0 开始，有的接着上一段，
            // 有的回退），所以把这条流在本分片里的第一个包对齐到全局时间轴的
            // 末尾，本分片后续的包跟着平移同样的量。平移量对 pts/dts 相同，
            // 两者之差（B 帧的显示顺序）保持不变。
            offsets_[index] = timeline_next_dts_[index] - dts;
            first_packet[index] = false;
        }
        pkt->dts = dts + offsets_[index];
        pkt->pts = pts + offsets_[index];

        // 源给的包时长未必有值，没有就按一个时基单位推进：目的只是让全局时间轴
        // 严格向前，下一份分片的平移量会在此基础上重新对齐。
        const int64_t duration = pkt->duration > 0 ? pkt->duration : 1;
        timeline_next_dts_[index] = pkt->dts + duration;

        pkt->pos = -1;   // 内存输入的位置对输出没有意义
        pkt->stream_index = static_cast<int>(index);
        // movenc 会在写头时给每条轨挑自己的 timescale，输出流的 time_base
        // 与输入的通常并不相等，所以写之前必须按两者重采样。
        av_packet_rescale_ts(pkt, input->streams[input_index]->time_base,
                             output_->streams[index]->time_base);
        // 复用器要求同一条流的 dts 严格递增。重采样是舍入的，极窄的时间间隔
        // 可能被压平，所以在交出去之前再确认一次。
        if (pkt->dts <= last_written_dts_[index]) {
            pkt->dts = last_written_dts_[index] + 1;
            if (pkt->pts < pkt->dts) {
                pkt->pts = pkt->dts;
            }
        }
        last_written_dts_[index] = pkt->dts;

        // 写成功时包已经被复用器接管并置空；失败时它会留下内容，由调用方放掉。
        return av_interleaved_write_frame(output_, pkt);
    };

    // 出错时的统一收尾：包与输入都还回去，已攒的半截输出清掉 —— 它没有意义，
    // 留着只会让上层的分片内容对不上。
    auto fail = [&](AVPacket** pkt, int code, const char* what) -> bool {
        av_packet_unref(*pkt);
        av_packet_free(pkt);
        release_input(&input, &input_io);
        release_pending(&pending_packets_);
        buffer_.clear();
        sink_.pos = 0;
        *error = std::string(what) + av_error_text(code);
        return false;
    };

    while (true) {
        // 探测阶段读过的包被 libavformat 缓在内部队列里，这里会原样吐出来，
        // 分片开头不会丢。
        if (pending_index < pending_packets_.size()) {
            av_packet_move_ref(packet, pending_packets_[pending_index]);
            ++pending_index;
        } else if (av_read_frame(input, packet) < 0) {
            break;   // EOF 或尾部损坏都当结束
        }
        const unsigned int input_index = static_cast<unsigned int>(packet->stream_index);
        if (input_index >= stream_map_.size() || stream_map_[input_index] < 0) {
            av_packet_unref(packet);
            continue;   // 被丢掉的流（字幕、数据流）
        }
        const std::size_t index = static_cast<std::size_t>(stream_map_[input_index]);

        AVBSFContext* filter = bsf_[index];
        if (filter == nullptr) {
            const int write_ret = emit_packet(packet, index, input_index);
            if (write_ret < 0) {
                return fail(&packet, write_ret, "写 fMP4 分片失败：");
            }
            continue;
        }

        // 送进码流过滤器。send 成功时包的所有权归过滤器，调用方这个对象被置空。
        int ret = av_bsf_send_packet(filter, packet);
        if (ret < 0) {
            return fail(&packet, ret, "音频包交给 aac_adtstoasc 失败：");
        }
        // 把过滤器能吐的都取走。aac_adtstoasc 是一进一出，循环是为了不漏 ——
        // 过滤器内部可能压着上一轮的包。EAGAIN 表示它还要更多输入。
        while (true) {
            ret = av_bsf_receive_packet(filter, packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                return fail(&packet, ret, "从 aac_adtstoasc 取包失败：");
            }
            const int write_ret = emit_packet(packet, index, input_index);
            if (write_ret < 0) {
                return fail(&packet, write_ret, "写 fMP4 分片失败：");
            }
            av_packet_unref(packet);
        }
    }

    av_packet_free(&packet);
    release_pending(&pending_packets_);
    release_input(&input, &input_io);

    // 1) 把交织器里还没吐出来的包排空（它会为了排序把它们压着）。
    const int drain_ret = av_interleaved_write_frame(output_, nullptr);
    if (drain_ret < 0) {
        buffer_.clear();
    sink_.pos = 0;
        *error = "写 fMP4 分片失败（排空交织队列）：" + av_error_text(drain_ret);
        return false;
    }
    // 2) 强制结束当前 fragment。movenc 默认要等到下一个关键帧才把 moof 写出来，
    //    不强制的话这一段的字节会被算到下一份分片的名字底下 —— 切分整体错位
    //    一格，播放器拿到的全是别人的内容。hls 复用器切段时做的也是这一步。
    av_write_frame(output_, nullptr);
    avio_flush(output_io_);

    const std::size_t after = buffer_.size();
    out->assign(buffer_.begin() + static_cast<std::ptrdiff_t>(before),
                buffer_.begin() + static_cast<std::ptrdiff_t>(after));
    // 输出字节只留到被取走为止：init 段已经单独存了一份，分片的字节由调用方
    // 持有，这里没必要把整部片子的输出一直攒着。
    buffer_.clear();
    sink_.pos = 0;
    return true;
}

}  // namespace adisplay::pipeline
