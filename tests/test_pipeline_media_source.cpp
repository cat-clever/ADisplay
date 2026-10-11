// ADisplay —— 媒体源形态判断的测试
//
// 守的是「该不该在本地换封装」这个判断。判断错一次，要么本来能直接播的片源
// 被多绕一层本地 relay，要么该换封装的没换、用户看到的是有声音没画面。
//
// 测试里的两段正文都取自真实抓到的播放列表：一段是手机本地代理给的
// HEVC-in-TS（AVPlayer 播不了视频轨），另一段是常见的 fMP4 形态。

#include <adisplay/pipeline/MediaSource.h>

#include <string>

#include "AdTest.h"

namespace {

using adisplay::pipeline::SegmentFormat;

SegmentFormat classify(const std::string& body) {
    return adisplay::pipeline::classify_playlist(body);
}

}  // namespace

AD_TEST(media_source_real_ts_playlist, "手机本地代理那种 TS 播放列表判为 MpegTs") {
    // 真实抓到的形态：分片是相对地址且带签名，长度在 EXTINF 里。
    const std::string body =
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
    AD_CHECK(classify(body) == SegmentFormat::MpegTs);
    AD_CHECK(adisplay::pipeline::needs_local_remux(SegmentFormat::MpegTs));
}

AD_TEST(media_source_fmp4_playlist, "fMP4 播放列表判为 FragmentedMp4，不需要换封装") {
    const std::string body =
        "#EXTM3U\n"
        "#EXT-X-VERSION:7\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXTINF:6.000000,\n"
        "seg-1.m4s\n"
        "#EXTINF:6.000000,\n"
        "seg-2.m4s\n";
    AD_CHECK(classify(body) == SegmentFormat::FragmentedMp4);
    AD_CHECK(!adisplay::pipeline::needs_local_remux(SegmentFormat::FragmentedMp4));
}

AD_TEST(media_source_master_playlist, "主列表要再拉一层才知道") {
    const std::string body =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1296893,RESOLUTION=1280x536\n"
        "1000k/index.m3u8\n";
    AD_CHECK(classify(body) == SegmentFormat::MasterPlaylist);
    // 认不出来时绝不能换封装 —— 宁可多拉一次。
    AD_CHECK(!adisplay::pipeline::needs_local_remux(SegmentFormat::MasterPlaylist));
}

AD_TEST(media_source_unknown_stays_unknown, "认不出来就交给播放器，不猜") {
    AD_CHECK(classify("") == SegmentFormat::Unknown);
    AD_CHECK(classify("#EXTM3U\n#EXT-X-ENDLIST\n") == SegmentFormat::Unknown);
    AD_CHECK(!adisplay::pipeline::needs_local_remux(SegmentFormat::Unknown));
}

AD_TEST(media_source_extension_case_and_query, "扩展名大小写与查询串都要认") {
    AD_CHECK(classify("#EXTM3U\nseg.TS\n") == SegmentFormat::MpegTs);
    AD_CHECK(classify("#EXTM3U\nseg.m4s?a=1&b=2\n") == SegmentFormat::FragmentedMp4);
}

AD_TEST(media_source_absolute_segment_url, "绝对地址的分片也要认得出来") {
    const std::string body =
        "#EXTM3U\n"
        "#EXTINF:5.0,\n"
        "http://172.20.10.2:7000/output_000000.ts?m8=abc\n";
    AD_CHECK(classify(body) == SegmentFormat::MpegTs);
}

AD_TEST(media_source_looks_like_hls, "只有 .m3u8/.m3u 才当作播放列表去拉") {
    using adisplay::pipeline::looks_like_hls;
    AD_CHECK(looks_like_hls("http://172.20.10.2:7000/resource.m3u8?src=xxx"));
    AD_CHECK(looks_like_hls("http://a.b/c/index.M3U8"));
    AD_CHECK(looks_like_hls("http://a.b/c/index.m3u"));
    // B 站那种直连 mp4 —— 连拉都不用拉，原样交给播放器。
    AD_CHECK(!looks_like_hls("http://cn-zjjh.bilivideo.com/x.mp4?e=1&uipk=5"));
    AD_CHECK(!looks_like_hls("http://a.b/c/segment.ts"));
}

int main() {
    return adtest::run_all("媒体源形态测试");
}
