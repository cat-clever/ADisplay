// ADisplay —— MPEG-TS 分片到 fMP4 的换封装（libavformat）
//
// 只换封装，不解码：HEVC / H.264 的码流原样搬过去，起始码与长度前缀由 mp4
// 复用器自己处理（ffmpeg -i in.ts -c copy out.mp4 走的就是这条路，不需要任何
// bitstream filter）。所以这里只依赖 libavformat / libavcodec / libavutil，
// 不碰 swscale / swresample，也不碰编码器。
//
// 一个会话共用一个实例：复用器内部持有 moov 与各条轨的 timescale，每个分片都
// 重开一个的话，每个分片会各带一份 moov，时间戳也会回到零点。
//
// 本类不做同步 —— 调用方（RelaySession）保证同一时刻只有一个线程在调它。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVIOContext;
struct AVBSFContext;
struct AVPacket;

namespace adisplay::pipeline {

// 输出字节的落脚点：一块内存，但像文件一样有「当前位置」。
//
// 类型放在这里而不是 .cpp 里，是因为写/定位回调是 .cpp 中的自由函数，需要
// 这个类型名；而回调拿到的正是它的地址，所以地址必须由持有者保证稳定。
// 它不是对外接口的一部分，只是没有别的地方可放。
struct OutputSink {
    std::vector<uint8_t>* bytes = nullptr;
    std::size_t pos = 0;
};

class Mp4Remuxer {
public:
    Mp4Remuxer();
    ~Mp4Remuxer();

    Mp4Remuxer(const Mp4Remuxer&) = delete;
    Mp4Remuxer& operator=(const Mp4Remuxer&) = delete;

    // 把一段 TS 字节换成 fMP4，out 收到这一段新增的 moof+mdat 字节。
    // 第一次调用会顺带建好输出流并写出 ftyp+moov，可以用 init_segment() 取走。
    // 同一份源的分片必须按播放顺序调用 —— 时间戳是一条道走到底的。
    bool append_ts_segment(const uint8_t* data, std::size_t size, std::vector<uint8_t>* out,
                           std::string* error);

    // ftyp+moov。第一个分片成功写完之后才有内容。
    const std::vector<uint8_t>& init_segment() const { return init_segment_; }
    bool has_init_segment() const { return has_init_; }

private:
    bool ensure_output(const AVFormatContext* input, std::string* error);

    // 写 ftyp+moov 之前先把 AAC 的解码配置取出来。
    //
    // 必须在写头之前：movenc 写 moov 时就要把 AudioSpecificConfig 放进 esds，
    // 而那份配置要等第一条音频包流过 aac_adtstoasc 才会有。顺序反了（先写头、
    // 后收包），esds 就是残缺的，播放器会拒绝打开整个文件。
    void prime_aac_config(AVFormatContext* input);
    void close_output();

    AVFormatContext* output_ = nullptr;
    AVIOContext* output_io_ = nullptr;
    std::vector<uint8_t> buffer_;              // 输出字节的落脚点，写回调往这里写
    OutputSink sink_;                          // 与 buffer_ 配套的当前位置
    std::vector<uint8_t> init_segment_;
    // 预读阶段攒下的包。它们是这个分片开头的帧 —— 丢掉的话每个分片都会缺
    // 一小段声音，所以先留着，写完头之后按原顺序处理。
    std::vector<AVPacket*> pending_packets_;
    // 每条输入流的 AAC 解码配置（AudioSpecificConfig）。空表示没取到。
    std::vector<std::vector<uint8_t>> aac_config_;

    // 每条输出流的码流过滤器（下标与输出流对齐），nullptr 表示这条流不需要。
    // 目前只有 AAC 用得上：TS 里它是 ADTS 封装，而 MP4 要裸 AAC。
    std::vector<AVBSFContext*> bsf_;
    std::vector<int> stream_map_;              // 输入流下标 → 输出流下标，-1 表示丢弃
    std::vector<int64_t> offsets_;             // 每条输出流在当前分片上的时间戳平移量
    std::vector<int64_t> timeline_next_dts_;   // 每条输出流的全局时间轴末尾（输入时基）
    std::vector<int64_t> last_written_dts_;    // 已写出的最后一个 dts（输出时基），保严格递增
    bool has_init_ = false;
    bool header_written_ = false;
};

}  // namespace adisplay::pipeline
