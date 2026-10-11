#include <adisplay/common/ByteOrder.h>

namespace adisplay::common {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

// 单字符转 0-15，非法返回 -1。
int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

std::string to_hex_string(const uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return std::string();
    }
    std::string out;
    out.reserve(size * 3u);
    for (std::size_t i = 0; i < size; ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        out.push_back(kHexDigits[(data[i] >> 4) & 0x0Fu]);
        out.push_back(kHexDigits[data[i] & 0x0Fu]);
    }
    return out;
}

std::vector<uint8_t> from_hex_string(const std::string& text) {
    std::vector<uint8_t> out;
    out.reserve(text.size() / 2u);

    int high = -1;
    for (const char c : text) {
        if (c == ' ' || c == ':' || c == '-' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        const int value = hex_value(c);
        if (value < 0) {
            return std::vector<uint8_t>();  // 非法字符，整串作废
        }
        if (high < 0) {
            high = value;
        } else {
            out.push_back(static_cast<uint8_t>((high << 4) | value));
            high = -1;
        }
    }

    if (high >= 0) {
        return std::vector<uint8_t>();  // 落单的半字节
    }
    return out;
}

}  // namespace adisplay::common
