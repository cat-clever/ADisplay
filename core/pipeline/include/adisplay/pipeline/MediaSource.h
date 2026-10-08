// ADisplay —— 媒体源的形态判断
//
// 批次 3 的媒体管线刻意不做解码与渲染：DLNA 给的是一条 URL，解码、渲染、
// 音画同步全交给各平台自带的播放器。这里只处理一件事 —— 播放器吃不下某种
// 封装时，我们得知道要接手。
//
// 具体的坑：Apple 的 HLS 规范要求 HEVC 用 fMP4（CMAF）封装，AVPlayer
// 不支持 HEVC-in-TS。而国内不少投屏 App 的本地代理是直通源编码的（H.265
// 转码太贵，手机上不会做），推过来的就是 HEVC-in-TS —— AVPlayer 能解析
// 播放列表、能算出时长（所以进度条能拖），但视频轨直接丢掉，表现为
// 「有声音没画面」。这种情况要在本地换封装后再喂给它。
//
// 纯函数，单独成文件是为了能被单元测试直接覆盖：判断错一次，要么本来能播的
// 片源被多绕一层本地 relay，要么该换封装的没换、用户看到黑屏。
#pragma once

#include <string>

namespace adisplay::pipeline {

// 播放列表里分片的封装格式。
enum class SegmentFormat {
    // 认不出来。调用方应当走「原样交给系统播放器」这条默认路径 ——
    // 猜错会让本来能播的片源播不了，比多绕一层代价大得多。
    Unknown = 0,

    // MPEG-TS（.ts）。HEVC-in-TS 需要本地换封装。
    MpegTs = 1,

    // 分片 MP4（.m4s / .mp4 / 带 EXT-X-MAP 的 init 段）。已经是我们想要的
    // 形态，系统播放器直接能放。
    FragmentedMp4 = 2,

    // 主播放列表（EXT-X-STREAM-INF），真正的分片在它指向的子列表里。
    // 要再拉一次才能判断。
    MasterPlaylist = 3,
};

// 从播放列表正文判断分片封装。
//
// 只看第一个分片行的扩展名，并识别 #EXT-X-MAP（fMP4 的初始化段）与
// #EXT-X-STREAM-INF（主列表）。认不出来就返回 Unknown。
SegmentFormat classify_playlist(const std::string& body);

// 这个地址看起来是不是 HLS 播放列表。不是的话连拉都不用拉，直接交给播放器。
bool looks_like_hls(const std::string& url);

// 该格式是否需要本地换封装才能交给系统播放器。
bool needs_local_remux(SegmentFormat format);

}  // namespace adisplay::pipeline
