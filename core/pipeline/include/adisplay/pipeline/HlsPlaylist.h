// ADisplay —— HLS 播放列表的本地改写
//
// 这里只做「文本进、文本出」的纯逻辑，不碰网络也不碰 FFmpeg，单独成文件是为了
// 能被单元测试直接覆盖：这几十行决定了本地播放列表里分片的地址、序号与时长，
// 错一处就表现为「播放列表能解析、播放器却一直转圈」或者「画面从中间开始」。
//
// 背景见 MediaSource.h：手机投屏 App 给的是 HEVC-in-TS 的播放列表，AVPlayer
// 放不了它的视频轨，要在本地换成 fMP4 再喂给播放器。换封装后的分片落在本地
// HTTP 服务上，本地播放列表就是这些分片的清单。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace adisplay::pipeline {

// 一条分片记录。url 已经按播放列表所在地址解析成绝对地址 —— relay 要拿它去
// 下载，而源里普遍是相对地址（output_000000.ts?m8=...&sz=...）。
struct HlsSegment {
    double duration = 0.0;
    std::string url;
};

// 把 reference 相对 base_url 解析成绝对地址。已经是绝对地址时原样返回。
// reference 里的查询串要完整保留 —— 手机本地代理的签名就在查询串里，
// 丢掉一个字符远端就回 403。
std::string resolve_relative_url(const std::string& base_url, const std::string& reference);

// 解析媒体播放列表里的分片序列。下列情况返回空：
//   * 没有分片行，或者分片行前面没有 #EXTINF（拿不到时长就没法排时间轴）
//   * 用了 #EXT-X-BYTERANGE：分片是同一个文件的字节区间，按整段下载会拿到错内容
//   * 用了 #EXT-X-KEY 且 METHOD 不是 NONE：内容是加密的，换封装只会得到乱码
// 返回空时调用方应当退回远端地址，而不是当「没有分片」播出去。
std::vector<HlsSegment> parse_media_playlist(const std::string& body,
                                             const std::string& playlist_url);

// 主列表里第一个子列表的绝对地址。不是主列表（没有 #EXT-X-STREAM-INF，
// 或者没有子列表行）时返回空。
std::string first_variant_url(const std::string& body, const std::string& playlist_url);

// 本地 VOD 播放列表。
struct LocalVodPlaylist {
    std::string body;                       // 空表示生成失败
    std::vector<std::string> segment_urls;  // 与 seg-<n>.m4s 一一对应的远端地址

    // 每份分片的起始时刻（毫秒），由前面所有分片的 EXTINF 累加得来。
    //
    // 中转跳转时要用它：换封装的时间轴是「一条道走到底」的，从中间某一份重新
    // 开始时必须把时间轴抬到它的起始时刻，否则新分片的时间戳会退回到零，
    // 播放器那边就是一次时间轴断裂。取 EXTINF 的累加值是准的 —— 播放器自己
    // 也是按这些 EXTINF 算时间轴的。
    std::vector<int64_t> segment_start_ms;
};

// 按远端播放列表正文生成一份指向本地 init.mp4 / seg-<n>.m4s 的 VOD 播放列表。
LocalVodPlaylist build_local_vod_playlist(const std::string& remote_body,
                                          const std::string& remote_url);

// 本地分片的命名。播放列表里写的名字与 HTTP 路由必须一致，所以只在这里定义一次。
std::string local_init_name();
std::string local_segment_name(std::size_t index);

// 把地址编码进查询串（relay 用它拼 local.m3u8?src=...）。
// 只放行 RFC 3986 的 unreserved 字符，其余一律百分号编码 —— 远端地址里的
// "://"、"?"、"&" 不编码的话，播放器会把它们当成我们这条本地 URL 的结构。
std::string encode_url_parameter(const std::string& value);

}  // namespace adisplay::pipeline
