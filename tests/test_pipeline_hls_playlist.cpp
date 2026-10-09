// ADisplay —— 本地 HLS 播放列表生成的测试
//
// 守的是「播放器拿到的那份列表对不对」：分片地址（相对 / 绝对 / 带签名）、
// 序号与命名、时长与 TARGETDURATION。错一处都不会报错，只会表现为播放器一直
// 转圈，或者能播但进度条起点不对 —— 排查时会以为是网络问题。
//
// 正文取自真实抓到的播放列表：手机投屏 App 在它自己手机上跑了个带签名的本地
// 代理，播放列表里的分片是 output_000000.ts?m8=...&sz=... 这种形态。

#include <adisplay/pipeline/HlsPlaylist.h>

#include <string>
#include <vector>

#include "AdTest.h"

namespace {

using adisplay::pipeline::build_local_vod_playlist;
using adisplay::pipeline::encode_url_parameter;
using adisplay::pipeline::first_variant_url;
using adisplay::pipeline::HlsSegment;
using adisplay::pipeline::local_init_name;
using adisplay::pipeline::local_segment_name;
using adisplay::pipeline::LocalVodPlaylist;
using adisplay::pipeline::parse_media_playlist;
using adisplay::pipeline::resolve_relative_url;

const char* kRemoteUrl = "http://172.20.10.2:7000/resource.m3u8?src=xxx&t=1";

// 真实抓到的那份：两段分片，相对地址且带签名。
std::string real_ts_playlist() {
    return
        "#EXTM3U\n"
        "#EXT-X-VERSION:6\n"
        "#EXT-X-TARGETDURATION:11\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXT-X-PLAYLIST-TYPE:VOD\n"
        "#EXT-X-INDEPENDENT-SEGMENTS\n"
        "#EXTINF:7.680000,\n"
        "output_000000.ts?m8=63b8ac135a965f1d&sz=295348&resource=7E9730AB\n"
        "#EXTINF:5.000000,\n"
        "output_000001.ts?m8=1008c285880afbb7&sz=211312&resource=7E9730AB\n";
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

AD_TEST(hls_resolve_relative_urls, "相对、绝对、协议相对与查询串都要拼对") {
    const std::string base = "http://172.20.10.2:7000/live/resource.m3u8?src=xxx";

    // 同目录下的相对地址，签名查询串要原样带着。
    AD_CHECK_EQ(resolve_relative_url(base, "output_000000.ts?m8=abc&sz=1"),
                std::string("http://172.20.10.2:7000/live/output_000000.ts?m8=abc&sz=1"));
    // 子目录。
    AD_CHECK_EQ(resolve_relative_url(base, "1000k/index_000.ts"),
                std::string("http://172.20.10.2:7000/live/1000k/index_000.ts"));
    // 上级目录。
    AD_CHECK_EQ(resolve_relative_url(base, "../other/a.ts"),
                std::string("http://172.20.10.2:7000/other/a.ts"));
    // 以 / 开头的绝对路径：只借用主机，丢掉基准的目录。
    AD_CHECK_EQ(resolve_relative_url(base, "/root/b.ts"),
                std::string("http://172.20.10.2:7000/root/b.ts"));
    // 已经是绝对地址：原样。
    AD_CHECK_EQ(resolve_relative_url(base, "https://cdn.example.com/x/y.m4s?k=v"),
                std::string("https://cdn.example.com/x/y.m4s?k=v"));
    // 协议相对地址：借用基准的协议。
    AD_CHECK_EQ(resolve_relative_url(base, "//cdn.example.com/e/a.ts"),
                std::string("http://cdn.example.com/e/a.ts"));
    // 基准自己带查询串时，它的查询串不能进结果。
    AD_CHECK_EQ(resolve_relative_url("http://h/p/a.m3u8?src=/x/y", "b.ts"),
                std::string("http://h/p/b.ts"));
    // 空引用不猜。
    AD_CHECK_EQ(resolve_relative_url(base, ""), std::string());
}

AD_TEST(hls_parse_real_ts_playlist, "真实 TS 播放列表能解析出分片与时长") {
    const std::vector<HlsSegment> segments = parse_media_playlist(real_ts_playlist(), kRemoteUrl);
    AD_CHECK_EQ(segments.size(), static_cast<std::size_t>(2));
    AD_CHECK_EQ(segments[0].url,
                std::string("http://172.20.10.2:7000/output_000000.ts?m8=63b8ac135a965f1d"
                            "&sz=295348&resource=7E9730AB"));
    AD_CHECK_EQ(segments[1].url,
                std::string("http://172.20.10.2:7000/output_000001.ts?m8=1008c285880afbb7"
                            "&sz=211312&resource=7E9730AB"));
    AD_CHECK(segments[0].duration > 7.6 && segments[0].duration < 7.7);
    AD_CHECK(segments[1].duration > 4.9 && segments[1].duration < 5.1);
}

AD_TEST(hls_local_playlist_from_real_ts, "生成的本地 VOD 播放列表逐行正确") {
    const LocalVodPlaylist local = build_local_vod_playlist(real_ts_playlist(), kRemoteUrl);

    const std::string expected =
        "#EXTM3U\n"
        "#EXT-X-VERSION:7\n"                      // 源里的 6 是给 TS 用的，fMP4 要 7
        "#EXT-X-TARGETDURATION:8\n"               // 最长 7.68 向上取整
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXT-X-PLAYLIST-TYPE:VOD\n"
        "#EXT-X-INDEPENDENT-SEGMENTS\n"           // 源里有就照抄
        "#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXTINF:7.680,\n"
        "seg-0.m4s\n"
        "#EXTINF:5.000,\n"
        "seg-1.m4s\n"
        "#EXT-X-ENDLIST\n";
    AD_CHECK_EQ(local.body, expected);

    // 分片地址是给 relay 下载用的，必须还是远端的绝对地址（带签名）。
    AD_CHECK_EQ(local.segment_urls.size(), static_cast<std::size_t>(2));
    AD_CHECK_EQ(local.segment_urls[0],
                std::string("http://172.20.10.2:7000/output_000000.ts?m8=63b8ac135a965f1d"
                            "&sz=295348&resource=7E9730AB"));
    AD_CHECK(!contains(local.body, "output_000000.ts"));   // 本地列表里不该再出现远端地址
    AD_CHECK_EQ(local_init_name(), std::string("init.mp4"));
}

AD_TEST(hls_local_playlist_segment_start_times, "每份分片的起始时刻按 EXTINF 累加") {
    const LocalVodPlaylist local = build_local_vod_playlist(real_ts_playlist(), kRemoteUrl);

    // 中转跳转要靠这个值把换封装的时间轴抬到目标那一份的起始时刻。
    // 算错了就是时间轴不连续 —— 表现是拖完进度条卡住或者花屏，而这条路径
    // 光看日志是看不出来的（日志只会说「跳转到第 N 份」）。
    AD_CHECK_EQ(local.segment_start_ms.size(), static_cast<std::size_t>(2));
    AD_CHECK_EQ(local.segment_start_ms[0], static_cast<int64_t>(0));
    // 第一份 7.68 秒。
    AD_CHECK_EQ(local.segment_start_ms[1], static_cast<int64_t>(7680));
}

AD_TEST(hls_local_playlist_absolute_segments, "源里是绝对地址时也要能生成") {
    const std::string body =
        "#EXTM3U\n"
        "#EXT-X-VERSION:6\n"
        "#EXTINF:4.0,\n"
        "http://172.20.10.2:7000/output_000000.ts?m8=abc\n"
        "#EXTINF:4.0,\n"
        "http://172.20.10.2:7000/output_000001.ts?m8=def\n";
    const LocalVodPlaylist local = build_local_vod_playlist(body, "http://172.20.10.2:7000/a.m3u8");

    AD_CHECK_EQ(local.segment_urls[1],
                std::string("http://172.20.10.2:7000/output_000001.ts?m8=def"));
    AD_CHECK(contains(local.body, "#EXTINF:4.000,\nseg-1.m4s\n"));
    // 源里没有 EXT-X-INDEPENDENT-SEGMENTS，本地列表也不该自己加上。
    AD_CHECK(!contains(local.body, "#EXT-X-INDEPENDENT-SEGMENTS"));
    AD_CHECK(contains(local.body, "#EXT-X-TARGETDURATION:4\n"));
}

AD_TEST(hls_local_playlist_tolerates_crlf_and_junk, "换行与空白不影响解析") {
    const std::string body =
        "#EXTM3U\r\n"
        "#EXT-X-VERSION:6\r\n"
        "\r\n"
        "# some comment\r\n"
        "#EXTINF:6.000000,\r\n"
        "  seg_a.ts?m8=1  \r\n"
        "#EXTINF:6.000000,\r\n"
        "seg_b.ts?m8=2\r\n";
    const LocalVodPlaylist local = build_local_vod_playlist(body, "http://h:1/a.m3u8");

    AD_CHECK_EQ(local.segment_urls.size(), static_cast<std::size_t>(2));
    AD_CHECK_EQ(local.segment_urls[0], std::string("http://h:1/seg_a.ts?m8=1"));
    AD_CHECK_EQ(local.segment_urls[1], std::string("http://h:1/seg_b.ts?m8=2"));
    AD_CHECK_EQ(local.body.find("# some comment"), std::string::npos);
}

AD_TEST(hls_refuses_unsupported_playlists, "字节区间、加密与没有分片的列表一律不接手") {
    // 字节区间：分片是同一个文件的片段，按整段下载会拿到错内容。
    const std::string byterange =
        "#EXTM3U\n"
        "#EXT-X-VERSION:6\n"
        "#EXTINF:4.0,\n"
        "#EXT-X-BYTERANGE:1000@0\n"
        "all.ts\n";
    AD_CHECK(build_local_vod_playlist(byterange, "http://h:1/a.m3u8").body.empty());

    // 加密：换封装只会得到乱码。
    const std::string encrypted =
        "#EXTM3U\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"k.bin\"\n"
        "#EXTINF:4.0,\n"
        "a.ts\n";
    AD_CHECK(build_local_vod_playlist(encrypted, "http://h:1/a.m3u8").body.empty());

    // METHOD=NONE 表示没加密，要放行。
    const std::string plain =
        "#EXTM3U\n"
        "#EXT-X-KEY:METHOD=NONE\n"
        "#EXTINF:4.0,\n"
        "a.ts\n";
    AD_CHECK(!build_local_vod_playlist(plain, "http://h:1/a.m3u8").body.empty());

    // 没有 #EXTINF 的行不是分片；空正文与只有标签的正文都生不出列表。
    AD_CHECK(build_local_vod_playlist("", "http://h:1/a.m3u8").body.empty());
    AD_CHECK(build_local_vod_playlist("#EXTM3U\n#EXT-X-ENDLIST\n", "http://h:1/a.m3u8").body.empty());
    AD_CHECK(build_local_vod_playlist("#EXTM3U\na.ts\n", "http://h:1/a.m3u8").body.empty());
}

AD_TEST(hls_master_playlist_variant, "主列表要能取出第一条子列表") {
    const std::string body =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1296893,RESOLUTION=1280x536\n"
        "1000k/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000\n"
        "500k/index.m3u8\n";
    AD_CHECK_EQ(first_variant_url(body, "http://h:7000/live/master.m3u8"),
                std::string("http://h:7000/live/1000k/index.m3u8"));
    // 媒体播放列表里没有子列表，返回空。
    AD_CHECK_EQ(first_variant_url(real_ts_playlist(), kRemoteUrl), std::string());
    // 主列表本身解析不出分片 —— 所以 relay 必须先拉一层子列表。
    AD_CHECK(parse_media_playlist(body, "http://h:7000/live/master.m3u8").empty());
}

AD_TEST(hls_segment_naming, "本地分片命名必须与路由一致") {
    AD_CHECK_EQ(local_segment_name(0), std::string("seg-0.m4s"));
    AD_CHECK_EQ(local_segment_name(12), std::string("seg-12.m4s"));
}

AD_TEST(hls_encode_url_parameter, "远端地址拼进查询串时必须编码") {
    AD_CHECK_EQ(encode_url_parameter("http://172.20.10.2:7000/resource.m3u8?m8=1&sz=2"),
                std::string("http%3A%2F%2F172.20.10.2%3A7000%2Fresource.m3u8%3Fm8%3D1%26sz%3D2"));
    // unreserved 字符原样放行。
    AD_CHECK_EQ(encode_url_parameter("aZ09-_.~"), std::string("aZ09-_.~"));
    // 加号在查询串里有特殊含义，编码掉最稳。
    AD_CHECK_EQ(encode_url_parameter("a+b"), std::string("a%2Bb"));
}

int main() {
    return adtest::run_all("本地 HLS 播放列表测试");
}
