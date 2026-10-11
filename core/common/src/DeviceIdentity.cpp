#include <adisplay/common/DeviceIdentity.h>
#include <adisplay/common/Config.h>
#include <adisplay/common/Random.h>

#include <cstdint>

namespace adisplay::common {
namespace {

constexpr char kHexUpper[] = "0123456789ABCDEF";
constexpr char kHexLower[] = "0123456789abcdef";

bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

}  // namespace

std::string generate_device_id() {
    uint8_t bytes[6] = {0};
    if (!random_bytes(bytes, sizeof(bytes))) {
        // 随机源挂了。退化成一个固定但格式合法的值，让程序能起来，
        // 由上层日志去暴露问题 —— 总比直接崩溃强。
        bytes[0] = 0x02;
        bytes[1] = 0x00;
        bytes[2] = 0x00;
        bytes[3] = 0x00;
        bytes[4] = 0x00;
        bytes[5] = 0x01;
    }

    // 清掉多播位（bit 0）、置上本地管理位（bit 1）。
    bytes[0] = static_cast<uint8_t>((bytes[0] & 0xFCu) | 0x02u);

    std::string text;
    text.reserve(17);
    for (std::size_t i = 0; i < sizeof(bytes); ++i) {
        if (i != 0) {
            text.push_back(':');
        }
        text.push_back(kHexUpper[(bytes[i] >> 4) & 0x0Fu]);
        text.push_back(kHexUpper[bytes[i] & 0x0Fu]);
    }
    return text;
}

std::string generate_uuid_v4() {
    uint8_t bytes[16] = {0};
    if (!random_bytes(bytes, sizeof(bytes))) {
        return std::string();
    }

    // RFC 4122：版本号放第 7 字节高 4 位，变体放第 9 字节高 2 位。
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0Fu) | 0x40u);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3Fu) | 0x80u);

    std::string text;
    text.reserve(36);
    for (std::size_t i = 0; i < sizeof(bytes); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            text.push_back('-');
        }
        text.push_back(kHexLower[(bytes[i] >> 4) & 0x0Fu]);
        text.push_back(kHexLower[bytes[i] & 0x0Fu]);
    }
    return text;
}

bool is_valid_device_id(const std::string& text) {
    if (text.size() != 17) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool should_be_colon = (i % 3 == 2);
        if (should_be_colon) {
            if (text[i] != ':') {
                return false;
            }
        } else if (!is_hex(text[i])) {
            return false;
        }
    }
    return true;
}

bool is_valid_uuid(const std::string& text) {
    if (text.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool should_be_dash = (i == 8 || i == 13 || i == 18 || i == 23);
        if (should_be_dash) {
            if (text[i] != '-') {
                return false;
            }
        } else if (!is_hex(text[i])) {
            return false;
        }
    }
    return true;
}

DeviceIdentity DeviceIdentity::from_bytes(const uint8_t* six_bytes, const uint8_t* sixteen_bytes) {
    DeviceIdentity identity;

    if (six_bytes != nullptr) {
        std::string text;
        text.reserve(17);
        for (std::size_t i = 0; i < 6; ++i) {
            if (i != 0) {
                text.push_back(':');
            }
            text.push_back(kHexUpper[(six_bytes[i] >> 4) & 0x0Fu]);
            text.push_back(kHexUpper[six_bytes[i] & 0x0Fu]);
        }
        identity.device_id_ = text;
    }

    if (sixteen_bytes != nullptr) {
        std::string text;
        text.reserve(36);
        for (std::size_t i = 0; i < 16; ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) {
                text.push_back('-');
            }
            text.push_back(kHexLower[(sixteen_bytes[i] >> 4) & 0x0Fu]);
            text.push_back(kHexLower[sixteen_bytes[i] & 0x0Fu]);
        }
        identity.uuid_ = text;
    }

    return identity;
}

DeviceIdentity DeviceIdentity::load_or_create(const Config& config) {
    DeviceIdentity identity;

    identity.device_id_ = config.get_string(kConfigKeyDeviceId, std::string());
    if (!is_valid_device_id(identity.device_id_)) {
        // 配置里没有，或者被用户改坏了 —— 重新生成。
        identity.device_id_ = generate_device_id();
    }

    identity.uuid_ = config.get_string(kConfigKeyUuid, std::string());
    if (!is_valid_uuid(identity.uuid_)) {
        identity.uuid_ = generate_uuid_v4();
    }

    return identity;
}

void DeviceIdentity::store(Config& config) const {
    config.set_string(kConfigKeyDeviceId, device_id_);
    config.set_string(kConfigKeyUuid, uuid_);
}

}  // namespace adisplay::common
