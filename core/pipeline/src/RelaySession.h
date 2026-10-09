// ADisplay —— 一次本地中转会话
//
// 会话 = 一份远端播放列表定下来的分片清单 + 这些分片的按需生产与缓存。
//
// 同一时刻只有一个线程在生产：fMP4 的时间戳要一条道走到底（见 Mp4Remuxer），
// 乱序生产会让 dts 倒退，复用器直接报错。
//
// 跨度小的时候就顺序补 —— 谁先来谁当生产者，请求第 n 份分片的线程把前面还没产
// 的一并产出来，其余请求在同一把锁上等自己那一份就绪。这样既满足了「不打乱顺序」，
// 也满足了「同一份分片只下载一次、只换封装一次」。
//
// 跨度大时不这么做：顺序补的代价是要把中间每一份都下载、换封装一遍，用户拖一下
// 进度条跳到几百份之后，那就成了几百次网络往返加换封装 —— 播放器那边表现出来
// 就是「卡住不动」。所以大跨度直接从目标那一份重开一个换封装器，并把它的时间轴
// 起点抬到那一份的起始时刻（见 Mp4Remuxer::set_timeline_start_ms），
// 新旧分片的时间戳照样接得上。
//
// 请求可能早于播放列表到达的次序被打乱（播放器会先要 init 段、再要它当前需要
// 的那份分片），等待是被允许的：等不到就停不下来的是 stop() —— 它会把所有等待
// 者唤醒，让它们失败返回，而不是永久挂住。
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Mp4Remuxer.h"

namespace adisplay::pipeline {

class RelaySession {
public:
    // segment_start_ms 与 segment_urls 一一对应，是每份分片的起始时刻（毫秒）。
    // 可以传空 —— 那样跳转时的起点都按 0 算，时间戳会不连续，但不会崩。
    RelaySession(std::string playlist_url, std::vector<std::string> segment_urls,
                 std::vector<int64_t> segment_start_ms);
    ~RelaySession();

    RelaySession(const RelaySession&) = delete;
    RelaySession& operator=(const RelaySession&) = delete;

    // 取给播放器的 fMP4 初始化段。会把第 0 份分片一并产出来 —— 复用器得先拿到
    // 源流的编码参数才能建 moov，参数只能从分片里来。
    bool init_segment(std::vector<uint8_t>* out, std::string* error);

    // 取第 index 份分片换封装后的 moof+mdat。
    bool segment(std::size_t index, std::vector<uint8_t>* out, std::string* error);

    // 停止：唤醒所有在等的请求让它们失败返回，并释放已缓存的分片。
    // 幂等，析构时也会调用。正在下载的那一次请求要等它自己的超时结束才回来 ——
    // 那是唯一一处无法打断的等待（HTTP 读超时兜着，最长十秒）。
    void stop();
    bool stopped() const;

    std::size_t segment_count() const { return segment_urls_.size(); }
    const std::string& playlist_url() const { return playlist_url_; }

private:
    enum class State { Idle, Ready, Failed };

    struct Slot {
        State state = State::Idle;
        std::vector<uint8_t> data;  // 换封装好的 moof+mdat
        std::string error;
    };

    struct Produced {
        bool ok = false;     // 换封装成功，data 有效
        bool fatal = false;  // 复用器坏了：整个会话都不该再往下走
        std::vector<uint8_t> data;
        std::string error;
    };

    // 把下标 index 为止还没产的分片一次产完（失败过的跳过）。不持锁调用。
    // 跨度大时会先把生产位置跳到 index 再开始（见 restart_at）。
    void produce_until(std::size_t index);
    // 从 index 重新开始生产：换一个新的换封装器，并把时间轴起点抬到那一份的
    // 起始时刻。持锁调用（内部自己加锁）。
    void restart_at(std::size_t index);
    // 下载 + 换封装一份分片。不持锁调用，只由生产者线程调用。
    Produced produce_one(std::size_t index);

    bool wait_for_segment(std::size_t index, std::vector<uint8_t>* out, std::string* error);

    const std::string playlist_url_;
    const std::vector<std::string> segment_urls_;
    const std::vector<int64_t> segment_start_ms_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Slot> slots_;
    std::vector<uint8_t> init_segment_;
    std::size_t next_index_ = 0;  // 下一份要生产的分片
    bool has_init_ = false;
    bool producing_ = false;  // 有线程正在生产，同一时刻只允许一个
    bool stopping_ = false;
    bool broken_ = false;
    std::string broken_error_;
    std::string last_error_;
    // 只由生产者线程使用：生产是串行的，复用器不需要自己加锁。
    std::unique_ptr<Mp4Remuxer> remuxer_;
};

}  // namespace adisplay::pipeline
