#include <adisplay/pipeline/HlsPlaylist.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace adisplay::pipeline {
namespace {

std::string to_lower(const std::string& text) {
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

// 去掉首尾空白（含行尾的 \r）。
std::string trim(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           (text[end - 1] == '\r' || text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool starts_with(const std::string& text, const char* prefix) {
    return text.compare(0, std::strlen(prefix), prefix) == 0;
}

// 逐行遍历播放列表正文，空行跳过，行首行尾空白已去掉。
template <typename Fn>
void for_each_line(const std::string& body, Fn&& handler) {
    std::size_t pos = 0;
    while (pos <= body.size()) {
        std::size_t end = body.find('\n', pos);
        const bool is_last = end == std::string::npos;
        if (is_last) {
            end = body.size();
        }
        const std::string line = trim(body.substr(pos, end - pos));
        if (!line.empty()) {
            handler(line);
        }
        if (is_last) {
            break;
        }
        pos = end + 1;
    }
}

// 形如 scheme://…。协议头只认标准的 [A-Za-z][A-Za-z0-9+.-]*。
bool has_scheme(const std::string& text) {
    const std::size_t mark = text.find("://");
    if (mark == std::string::npos || mark == 0) {
        return false;
    }
    for (std::size_t i = 0; i < mark; ++i) {
        const char c = text[i];
        const bool is_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool is_tail = (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        if (!is_alpha && !(i > 0 && is_tail)) {
            return false;
        }
    }
    return true;
}

// 取 "scheme://主机[:端口]" 这一段，后面的路径与查询串丢掉。
std::string origin_of(const std::string& url) {
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return std::string();
    }
    const std::size_t authority_end = url.find_first_of("/?#", scheme_end + 3);
    return url.substr(0, authority_end == std::string::npos ? url.size() : authority_end);
}

// 把 reference 的路径段接到 base_path 的目录后面，顺便消掉 "." 与 ".."。
// 返回以 '/' 开头的路径。查询串与片段不在这里处理。
std::string merge_paths(const std::string& base_path, const std::string& reference) {
    std::vector<std::string> parts;
    std::string current;

    const auto flush = [&parts, &current]() {
        if (current.empty() || current == ".") {
            current.clear();
            return;
        }
        if (current == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
            current.clear();
            return;
        }
        parts.push_back(current);
        current.clear();
    };

    // 只用 base 的目录部分：最后一段文件名（resource.m3u8）要丢掉。
    const std::size_t slash = base_path.find_last_of('/');
    const std::string base_dir =
        slash == std::string::npos ? std::string() : base_path.substr(0, slash + 1);
    for (const char c : base_dir) {
        if (c == '/') {
            flush();
        } else {
            current.push_back(c);
        }
    }
    flush();

    for (const char c : reference) {
        if (c == '/') {
            flush();
        } else {
            current.push_back(c);
        }
    }
    flush();

    std::string merged = "/";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            merged.push_back('/');
        }
        merged += parts[i];
    }
    return merged;
}

// #EXTINF:7.680000, 里的时长。取不出来时返回 false。
bool parse_extinf_duration(const std::string& line, double* out_duration) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos || colon + 1 >= line.size()) {
        return false;
    }
    const char* begin = line.c_str() + colon + 1;
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    if (end == begin) {
        return false;
    }
    *out_duration = value;
    return true;
}

// #EXT-X-KEY: 是否表示内容被加密。METHOD=NONE 表示没加密，其余一律当作加密 ——
// 认不出来时宁可退回远端地址，也不要换封装出一堆乱码。
bool is_encrypted_key_line(const std::string& lowered_line) {
    const std::size_t at = lowered_line.find("method=");
    if (at == std::string::npos) {
        return true;
    }
    const std::size_t value_begin = at + std::strlen("method=");
    std::size_t value_end = lowered_line.find(',', value_begin);
    if (value_end == std::string::npos) {
        value_end = lowered_line.size();
    }
    return trim(lowered_line.substr(value_begin, value_end - value_begin)) != "none";
}

// 正文里有没有某个独立成行的标签。
bool has_tag_line(const std::string& body, const char* tag) {
    bool found = false;
    for_each_line(body, [&found, tag](const std::string& line) {
        if (starts_with(to_lower(line), tag)) {
            found = true;
        }
    });
    return found;
}

// 按 HLS 的惯例保留三位小数。源里是六位，这里少几位不影响播放：分片的真实
// 时间轴由它自己的 PTS 决定（见 Mp4Remuxer），EXTINF 只是给播放器排列表用的。
std::string format_duration(double duration) {
    if (!(duration > 0.0)) {   // 也把 NaN 挡在这里
        duration = 0.0;
    }
    char buffer[32] = {0};
    std::snprintf(buffer, sizeof(buffer), "%.3f", duration);
    return std::string(buffer);
}

}  // namespace

std::string resolve_relative_url(const std::string& base_url, const std::string& reference) {
    if (reference.empty()) {
        return std::string();
    }
    if (has_scheme(reference)) {
        return reference;   // 已经是绝对地址
    }
    const std::string origin = origin_of(base_url);
    if (origin.empty()) {
        return reference;   // 基准地址本身不是绝对地址，拼不出去就别拼
    }

    // 查询串与片段要原样带着走，路径部分才需要与基准拼。
    const std::size_t tail_at = reference.find_first_of("?#");
    const std::string path = reference.substr(0, tail_at);
    const std::string tail =
        tail_at == std::string::npos ? std::string() : reference.substr(tail_at);

    if (starts_with(reference, "//")) {
        // 协议相对地址：只借用基准的协议。
        const std::size_t scheme_end = base_url.find("://");
        return base_url.substr(0, scheme_end + 3) + reference.substr(2);
    }
    if (starts_with(path, "/")) {
        return origin + merge_paths("/", path) + tail;
    }

    // 相对地址按基准的目录解析。基准自己的查询串不参与 —— RFC 3986 里
    // 相对引用的解析结果不带基准的查询串。merge_paths 只吃路径，所以要把
    // "scheme://主机" 这一段先切掉。
    std::size_t base_end = base_url.find_first_of("?#");
    if (base_end == std::string::npos) {
        base_end = base_url.size();
    }
    const std::string base_path = base_url.substr(origin.size(), base_end - origin.size());
    return origin + merge_paths(base_path, path) + tail;
}

std::vector<HlsSegment> parse_media_playlist(const std::string& body,
                                             const std::string& playlist_url) {
    std::vector<HlsSegment> segments;
    if (body.empty()) {
        return segments;
    }

    double pending_duration = 0.0;
    bool have_pending = false;
    bool unsupported = false;

    for_each_line(body, [&](const std::string& line) {
        if (line[0] != '#') {
            // 非注释行就是分片地址。没有 #EXTINF 的行不是分片（例如
            // #EXT-X-MAP 的 URI 属性在注释行里，不会走到这里）。
            if (have_pending) {
                HlsSegment segment;
                segment.duration = pending_duration;
                segment.url = resolve_relative_url(playlist_url, line);
                segments.push_back(std::move(segment));
                have_pending = false;
            }
            return;
        }

        const std::string lowered = to_lower(line);
        if (starts_with(lowered, "#ext-x-byterange")) {
            unsupported = true;
        } else if (starts_with(lowered, "#ext-x-key")) {
            unsupported = unsupported || is_encrypted_key_line(lowered);
        } else if (starts_with(lowered, "#extinf")) {
            have_pending = parse_extinf_duration(line, &pending_duration);
        }
    });

    if (unsupported) {
        return std::vector<HlsSegment>();
    }
    return segments;
}

std::string first_variant_url(const std::string& body, const std::string& playlist_url) {
    std::string variant;
    bool expect_variant = false;
    for_each_line(body, [&](const std::string& line) {
        if (!variant.empty()) {
            return;
        }
        if (line[0] == '#') {
            if (starts_with(to_lower(line), "#ext-x-stream-inf")) {
                expect_variant = true;
            }
            return;
        }
        if (expect_variant) {
            variant = resolve_relative_url(playlist_url, line);
        }
    });
    return variant;
}

LocalVodPlaylist build_local_vod_playlist(const std::string& remote_body,
                                          const std::string& remote_url) {
    LocalVodPlaylist result;

    std::vector<HlsSegment> segments = parse_media_playlist(remote_body, remote_url);
    if (segments.empty()) {
        return result;
    }

    double longest = 0.0;
    for (const HlsSegment& segment : segments) {
        longest = std::max(longest, segment.duration);
    }
    // 规范要求 TARGETDURATION 不小于任何一条 EXTINF（四舍五入后）。
    int target_duration = static_cast<int>(std::ceil(longest));
    if (target_duration < 1) {
        target_duration = 1;
    }

    std::string body;
    body += "#EXTM3U\n";
    // 版本号要重写：源里那个是给 TS 用的，而 #EXT-X-MAP 要求 6 起、
    // CMAF 的 fMP4 分片要求 7。
    body += "#EXT-X-VERSION:7\n";
    body += "#EXT-X-TARGETDURATION:" + std::to_string(target_duration) + "\n";
    // 本地分片按位置从 0 编号，源里的 EXT-X-MEDIA-SEQUENCE 与它无关 ——
    // 我们只按顺序一一对应，序号是本地的。
    body += "#EXT-X-MEDIA-SEQUENCE:0\n";
    body += "#EXT-X-PLAYLIST-TYPE:VOD\n";
    if (has_tag_line(remote_body, "#ext-x-independent-segments")) {
        // 每个源分片开头都有随机访问点才敢照抄这个标签。
        body += "#EXT-X-INDEPENDENT-SEGMENTS\n";
    }
    body += "#EXT-X-MAP:URI=\"" + local_init_name() + "\"\n";
    for (std::size_t i = 0; i < segments.size(); ++i) {
        body += "#EXTINF:" + format_duration(segments[i].duration) + ",\n";
        body += local_segment_name(i) + "\n";
    }
    body += "#EXT-X-ENDLIST\n";

    result.body = std::move(body);
    result.segment_urls.reserve(segments.size());
    for (HlsSegment& segment : segments) {
        result.segment_urls.push_back(std::move(segment.url));
    }
    return result;
}

std::string local_init_name() {
    return "init.mp4";
}

std::string local_segment_name(std::size_t index) {
    return "seg-" + std::to_string(index) + ".m4s";
}

std::string encode_url_parameter(const std::string& value) {
    static const char kHexDigits[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const char raw : value) {
        const unsigned char c = static_cast<unsigned char>(raw);
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            encoded.push_back(raw);
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHexDigits[(c >> 4) & 0x0F]);
        encoded.push_back(kHexDigits[c & 0x0F]);
    }
    return encoded;
}

}  // namespace adisplay::pipeline
