#include <adisplay/common/DeviceName.h>

#include <cstdint>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <unistd.h>
#elif defined(__linux__)
#  include <unistd.h>
#endif

namespace adisplay::common {
namespace {

// 判断码点是否属于要过滤掉的「控制 / 格式」字符。
//
// 依据文档 2.4「自动过滤控制字符」。除了 C0/C1 与 DEL，还过滤掉一批
// 会破坏显示的不可见格式字符 —— 它们不是严格意义上的控制符，但混进
// 设备名后会让手机端列表出现看不见的空白或换行错觉。
bool is_filtered_codepoint(uint32_t cp) {
    if (cp <= 0x1Fu) {                       // C0 控制符
        return true;
    }
    if (cp == 0x7Fu) {                       // DEL
        return true;
    }
    if (cp >= 0x80u && cp <= 0x9Fu) {        // C1 控制符
        return true;
    }
    if (cp >= 0x200Bu && cp <= 0x200Fu) {    // 零宽空格 / 连接符 / 方向标记
        return true;
    }
    if (cp == 0x2028u || cp == 0x2029u) {    // 行分隔符 / 段分隔符
        return true;
    }
    if (cp == 0xFEFFu) {                     // BOM / 零宽不换行空格
        return true;
    }
    return false;
}

// 从 utf8_text[index] 起解出一个码点，index 前进到下一个字符。
// 非法序列返回 false 并把 index 推进 1 字节，避免死循环。
bool utf8_next(std::string_view utf8_text, std::size_t& index, uint32_t& codepoint) {
    const std::size_t size = utf8_text.size();
    if (index >= size) {
        return false;
    }

    const uint8_t b0 = static_cast<uint8_t>(utf8_text[index]);

    // 单字节：0xxxxxxx
    if (b0 < 0x80u) {
        codepoint = b0;
        ++index;
        return true;
    }

    std::size_t extra = 0;
    uint32_t value = 0;

    if ((b0 & 0xE0u) == 0xC0u) {          // 110xxxxx，2 字节
        extra = 1;
        value = b0 & 0x1Fu;
    } else if ((b0 & 0xF0u) == 0xE0u) {   // 1110xxxx，3 字节
        extra = 2;
        value = b0 & 0x0Fu;
    } else if ((b0 & 0xF8u) == 0xF0u) {   // 11110xxx，4 字节
        extra = 3;
        value = b0 & 0x07u;
    } else {
        // 0x80-0xBF 是孤立的续接字节，0xF8+ 是非法首字节。
        ++index;
        codepoint = 0xFFFDu;  // U+FFFD 替换字符
        return false;
    }

    if (index + extra >= size) {
        index = size;
        codepoint = 0xFFFDu;
        return false;
    }

    for (std::size_t i = 1; i <= extra; ++i) {
        const uint8_t bx = static_cast<uint8_t>(utf8_text[index + i]);
        if ((bx & 0xC0u) != 0x80u) {   // 必须是 10xxxxxx
            index += i;
            codepoint = 0xFFFDu;
            return false;
        }
        value = (value << 6) | (bx & 0x3Fu);
    }

    // 拒绝过长编码（overlong）与代理区 / 超范围码点。
    static const uint32_t kMinForLength[4] = {0u, 0x80u, 0x800u, 0x10000u};
    if (value < kMinForLength[extra] || value > 0x10FFFFu) {
        index += extra + 1;
        codepoint = 0xFFFDu;
        return false;
    }
    if (value >= 0xD800u && value <= 0xDFFFu) {   // UTF-16 代理区
        index += extra + 1;
        codepoint = 0xFFFDu;
        return false;
    }

    codepoint = value;
    index += extra + 1;
    return true;
}

// 把码点编回 UTF-8。
void utf8_append(std::string& out, uint32_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

// 是否为空白（用于裁掉首尾）。
bool is_trim_codepoint(uint32_t cp) {
    return cp == 0x20u || cp == 0x09u || cp == 0x0Au || cp == 0x0Du ||
           cp == 0xA0u || cp == 0x3000u;
}

}  // namespace

std::string sanitize_device_name(std::string_view utf8_name) {
    // 先解成码点序列：后续的「过滤」和「裁首尾空白」都在码点层面做，
    // 不会把一个多字节字符切成两半。
    std::vector<uint32_t> codepoints;
    codepoints.reserve(utf8_name.size());

    std::size_t index = 0;
    while (index < utf8_name.size()) {
        uint32_t cp = 0;
        utf8_next(utf8_name, index, cp);
        if (!is_filtered_codepoint(cp)) {
            codepoints.push_back(cp);
        }
    }

    // 裁掉首尾空白：设备名以空白开头，在手机端列表里会显得像排版错误。
    std::size_t begin = 0;
    std::size_t end = codepoints.size();
    while (begin < end && is_trim_codepoint(codepoints[begin])) {
        ++begin;
    }
    while (end > begin && is_trim_codepoint(codepoints[end - 1])) {
        --end;
    }

    std::string result;
    result.reserve((end - begin) * 3u);  // 中文按 3 字节估
    for (std::size_t i = begin; i < end; ++i) {
        utf8_append(result, codepoints[i]);
    }
    return result;
}

std::size_t utf8_codepoint_count(std::string_view utf8_text) {
    std::size_t count = 0;
    std::size_t index = 0;
    while (index < utf8_text.size()) {
        uint32_t cp = 0;
        utf8_next(utf8_text, index, cp);
        ++count;
    }
    return count;
}

std::string utf8_truncate(std::string_view utf8_text, std::size_t limit) {
    std::string out;
    out.reserve(utf8_text.size());

    std::size_t count = 0;
    std::size_t index = 0;
    while (index < utf8_text.size() && count < limit) {
        const std::size_t char_begin = index;
        uint32_t cp = 0;
        utf8_next(utf8_text, index, cp);
        out.append(utf8_text.data() + char_begin, index - char_begin);
        ++count;
    }
    return out;
}

DeviceNameValidation validate_device_name(std::string_view utf8_name) {
    DeviceNameValidation result;
    result.normalized = sanitize_device_name(utf8_name);

    if (result.normalized.empty()) {
        result.valid = false;
        result.reason = "设备名称不能为空";
        return result;
    }

    const std::size_t count = utf8_codepoint_count(result.normalized);
    if (count < kDeviceNameMinChars) {
        result.valid = false;
        result.reason = "设备名称不能为空";
        return result;
    }
    if (count > kDeviceNameMaxChars) {
        result.valid = false;
        result.reason = "设备名称最长 32 个字符（当前 " + std::to_string(count) + " 个）";
        return result;
    }

    result.valid = true;
    return result;
}

std::string dedupe_device_name(std::string_view name, const std::vector<std::string>& taken) {
    const std::string base(name);

    const auto is_taken = [&taken](const std::string& candidate) {
        for (const std::string& existing : taken) {
            if (existing == candidate) {
                return true;
            }
        }
        return false;
    };

    if (!is_taken(base)) {
        return base;
    }

    // 从 (2) 开始试。后缀最多长到 " (99)" 这种量级，局域网内不会更多了。
    for (int suffix = 2; suffix < 1000; ++suffix) {
        const std::string tail = " (" + std::to_string(suffix) + ")";
        const std::size_t tail_chars = tail.size();  // 全是 ASCII，字节数即码点数

        // 保留基名到「32 - 后缀长度」个码点，再拼后缀，保证总长不超限。
        std::string trimmed_base = base;
        if (utf8_codepoint_count(base) + tail_chars > kDeviceNameMaxChars) {
            trimmed_base = utf8_truncate(base, kDeviceNameMaxChars - tail_chars);
        }

        const std::string candidate = trimmed_base + tail;
        if (!is_taken(candidate)) {
            return candidate;
        }
    }

    // 理论上到不了这里。兜底返回原名，让上层日志里能看到异常。
    return base;
}

std::string default_device_name() {
#if defined(_WIN32)
    char buffer[256] = {0};
    DWORD size = static_cast<DWORD>(sizeof(buffer));
    if (::GetComputerNameA(buffer, &size) != 0 && size > 0) {
        return std::string(buffer, size);
    }
#elif defined(__APPLE__) || defined(__linux__)
    char buffer[256] = {0};
    if (::gethostname(buffer, sizeof(buffer) - 1) == 0 && buffer[0] != '\0') {
        std::string host(buffer);
        // macOS 上主机名常带 ".local" 后缀，手机端列表里显示出来很怪，去掉。
        const std::string suffix = ".local";
        if (host.size() > suffix.size() &&
            host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) {
            host.erase(host.size() - suffix.size());
        }
        if (!host.empty()) {
            return host;
        }
    }
#endif
    return "ADisplay";
}

}  // namespace adisplay::common
