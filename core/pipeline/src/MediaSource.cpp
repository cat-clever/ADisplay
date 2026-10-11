#include <adisplay/pipeline/MediaSource.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace adisplay::pipeline {
namespace {

std::string to_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool starts_with(const std::string& text, const char* prefix) {
    return text.compare(0, std::strlen(prefix), prefix) == 0;
}

// 去掉查询串与片段，只留路径部分 —— 分片地址普遍带签名，形如
// output_000000.ts?m8=...&sz=...，扩展名在问号之前。
std::string strip_query(const std::string& uri) {
    std::size_t cut = uri.find_first_of("?#");
    return cut == std::string::npos ? uri : uri.substr(0, cut);
}

bool ends_with(const std::string& text, const char* suffix) {
    const std::size_t suffix_len = std::strlen(suffix);
    return text.size() >= suffix_len &&
           text.compare(text.size() - suffix_len, suffix_len, suffix) == 0;
}

}  // namespace

bool looks_like_hls(const std::string& url) {
    const std::string path = to_lower(strip_query(url));
    if (ends_with(path, ".m3u8") || ends_with(path, ".m3u")) {
        return true;
    }
    // 手机本地代理常把播放列表伪装成资源名（resource.m3u8、index、playlist…），
    // 认不出来时宁可不认 —— 多拉一次播放列表的代价远小于误判。
    return false;
}

bool needs_local_remux(SegmentFormat format) {
    return format == SegmentFormat::MpegTs;
}

SegmentFormat classify_playlist(const std::string& body) {
    if (body.empty()) {
        return SegmentFormat::Unknown;
    }

    bool saw_map = false;      // #EXT-X-MAP → fMP4 的初始化段
    bool saw_stream_inf = false;

    std::size_t pos = 0;
    while (pos <= body.size()) {
        std::size_t end = body.find('\n', pos);
        if (end == std::string::npos) {
            end = body.size();
        }
        std::string line = body.substr(pos, end - pos);
        // 去掉行尾的 \r，以及首尾空白。
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                 line.back() == '\t')) {
            line.pop_back();
        }
        std::size_t begin = 0;
        while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) {
            ++begin;
        }
        line = line.substr(begin);

        if (line.empty()) {
            pos = end + 1;
            if (end == body.size()) {
                break;
            }
            continue;
        }

        if (line[0] == '#') {
            const std::string lowered = to_lower(line);
            if (starts_with(lowered, "#ext-x-map")) {
                saw_map = true;
            } else if (starts_with(lowered, "#ext-x-stream-inf")) {
                saw_stream_inf = true;
            }
            pos = end + 1;
            if (end == body.size()) {
                break;
            }
            continue;
        }

        // 第一个非注释行就是分片（或主列表指向的子列表）地址。
        const std::string uri = to_lower(strip_query(line));
        if (ends_with(uri, ".ts")) {
            return SegmentFormat::MpegTs;
        }
        if (ends_with(uri, ".m4s") || ends_with(uri, ".mp4") || ends_with(uri, ".cmfv") ||
            ends_with(uri, ".cmfa")) {
            return SegmentFormat::FragmentedMp4;
        }

        // 扩展名不认识。有 EXT-X-MAP 说明是 fMP4 体系，否则交给调用方再拉一层。
        if (saw_map) {
            return SegmentFormat::FragmentedMp4;
        }
        if (saw_stream_inf) {
            return SegmentFormat::MasterPlaylist;
        }
        return SegmentFormat::Unknown;
    }

    // 没有分片行。有 EXT-X-STREAM-INF 说明是主列表，等调用方拉子列表。
    if (saw_stream_inf) {
        return SegmentFormat::MasterPlaylist;
    }
    return SegmentFormat::Unknown;
}

}  // namespace adisplay::pipeline
