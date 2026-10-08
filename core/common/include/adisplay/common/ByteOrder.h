// ADisplay —— 字节序与位序读写工具
//
// 文档 2.5 的硬性要求：
//   「代码层面不做架构假设：使用定长整数类型，显式按字节序读写协议包头，
//     不直接把缓冲区强转为结构体；不写内联汇编。」
//
// 所以协议层的每一处包头解析都必须走这里，而不是 reinterpret_cast。
// 好处是：在大端机器上（部分 ARM 电视盒子）行为一致，且不受结构体填充影响。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace adisplay::common {

// ===========================================================================
// 按字节序读写整数
// ===========================================================================

inline uint16_t read_be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                                 static_cast<uint16_t>(p[1]));
}

inline uint32_t read_be24(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 16) |
           (static_cast<uint32_t>(p[1]) << 8) |
            static_cast<uint32_t>(p[2]);
}

inline uint32_t read_be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
            static_cast<uint32_t>(p[3]);
}

inline uint64_t read_be64(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(read_be32(p)) << 32) |
            static_cast<uint64_t>(read_be32(p + 4));
}

inline uint16_t read_le16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t read_le24(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16);
}

inline uint32_t read_le32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t read_le64(const uint8_t* p) noexcept {
    return static_cast<uint64_t>(read_le32(p)) |
           (static_cast<uint64_t>(read_le32(p + 4)) << 32);
}

inline void write_be16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[1] = static_cast<uint8_t>(v & 0xFFu);
}

inline void write_be24(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>(v & 0xFFu);
}

inline void write_be32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[3] = static_cast<uint8_t>(v & 0xFFu);
}

inline void write_be64(uint8_t* p, uint64_t v) noexcept {
    write_be32(p, static_cast<uint32_t>(v >> 32));
    write_be32(p + 4, static_cast<uint32_t>(v & 0xFFFFFFFFu));
}

inline void write_le16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

inline void write_le24(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
}

inline void write_le32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline void write_le64(uint8_t* p, uint64_t v) noexcept {
    write_le32(p, static_cast<uint32_t>(v & 0xFFFFFFFFu));
    write_le32(p + 4, static_cast<uint32_t>(v >> 32));
}

// 追加到 std::vector，协议层构造包头时比手算偏移安全。
inline void append_be16(std::vector<uint8_t>& out, uint16_t v) {
    const std::size_t base = out.size();
    out.resize(base + 2);
    write_be16(out.data() + base, v);
}

inline void append_be32(std::vector<uint8_t>& out, uint32_t v) {
    const std::size_t base = out.size();
    out.resize(base + 4);
    write_be32(out.data() + base, v);
}

inline void append_le16(std::vector<uint8_t>& out, uint16_t v) {
    const std::size_t base = out.size();
    out.resize(base + 2);
    write_le16(out.data() + base, v);
}

inline void append_le32(std::vector<uint8_t>& out, uint32_t v) {
    const std::size_t base = out.size();
    out.resize(base + 4);
    write_le32(out.data() + base, v);
}

// ===========================================================================
// 位读取器
//
// AirPlay 的镜像视频包头是按位打包的（载荷长度、载荷类型、时间戳挤在
// 一个 128 位头里），必须先按位取再解析。这里提供 MSB-first（网络位序）
// 与 LSB-first 两种取法，具体用哪种由协议层决定。
// ===========================================================================

class BitReader {
public:
    BitReader(const uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    // 剩余可读位数。
    std::size_t bits_left() const noexcept { return size_ * 8u - bit_pos_; }

    // 是否已经读完。
    bool exhausted() const noexcept { return bits_left() == 0u; }

    // 读 count 位，结果右对齐。count 取值 1..64。
    // 越界返回 0 并把 failed_ 置位，调用方用 failed() 判断。
    uint64_t read_msb(unsigned count) noexcept {
        uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            value = (value << 1) | static_cast<uint64_t>(next_bit_msb());
        }
        return value;
    }

    uint64_t read_lsb(unsigned count) noexcept {
        uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            value |= static_cast<uint64_t>(next_bit_lsb()) << i;
        }
        return value;
    }

    // 跳过若干位。
    void skip(std::size_t count) noexcept {
        if (bit_pos_ + count > size_ * 8u) {
            bit_pos_ = size_ * 8u;
            failed_ = true;
            return;
        }
        bit_pos_ += count;
    }

    // 读到字节边界（用于「位字段之后跟裸字节」的包头布局）。
    void align_to_byte() noexcept { bit_pos_ = (bit_pos_ + 7u) & ~static_cast<std::size_t>(7u); }

    bool failed() const noexcept { return failed_; }
    std::size_t bit_position() const noexcept { return bit_pos_; }

private:
    // MSB-first：从高位往低位走。
    unsigned next_bit_msb() noexcept {
        if (bit_pos_ >= size_ * 8u) {
            failed_ = true;
            return 0;
        }
        const std::size_t byte_index = bit_pos_ >> 3;
        const unsigned bit_index = 7u - static_cast<unsigned>(bit_pos_ & 7u);
        ++bit_pos_;
        return (data_[byte_index] >> bit_index) & 1u;
    }

    // LSB-first：从低位往高位走。
    unsigned next_bit_lsb() noexcept {
        if (bit_pos_ >= size_ * 8u) {
            failed_ = true;
            return 0;
        }
        const std::size_t byte_index = bit_pos_ >> 3;
        const unsigned bit_index = static_cast<unsigned>(bit_pos_ & 7u);
        ++bit_pos_;
        return (data_[byte_index] >> bit_index) & 1u;
    }

    const uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t bit_pos_ = 0;
    bool failed_ = false;
};

// 位写入器，构造响应包头时用。
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}

    void write_msb(uint64_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            const unsigned shift = count - 1u - i;
            push_bit_msb(static_cast<unsigned>((value >> shift) & 1u));
        }
    }

    void write_lsb(uint64_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            push_bit_lsb(static_cast<unsigned>((value >> i) & 1u));
        }
    }

    void align_to_byte() {
        while ((bit_pos_ & 7u) != 0u) {
            push_bit_msb(0u);
        }
    }

private:
    void push_bit_msb(unsigned bit) {
        const std::size_t byte_index = bit_pos_ >> 3;
        if (byte_index >= out_.size()) {
            out_.resize(byte_index + 1, 0);
        }
        const unsigned bit_index = 7u - static_cast<unsigned>(bit_pos_ & 7u);
        if (bit != 0u) {
            out_[byte_index] = static_cast<uint8_t>(out_[byte_index] | (1u << bit_index));
        } else {
            out_[byte_index] = static_cast<uint8_t>(out_[byte_index] & ~(1u << bit_index));
        }
        ++bit_pos_;
    }

    void push_bit_lsb(unsigned bit) {
        const std::size_t byte_index = bit_pos_ >> 3;
        if (byte_index >= out_.size()) {
            out_.resize(byte_index + 1, 0);
        }
        const unsigned bit_index = static_cast<unsigned>(bit_pos_ & 7u);
        if (bit != 0u) {
            out_[byte_index] = static_cast<uint8_t>(out_[byte_index] | (1u << bit_index));
        } else {
            out_[byte_index] = static_cast<uint8_t>(out_[byte_index] & ~(1u << bit_index));
        }
        ++bit_pos_;
    }

    std::vector<uint8_t>& out_;
    std::size_t bit_pos_ = 0;
};

// ===========================================================================
// 十六进制辅助（日志与抓包回放用，文档第 10 节）
// ===========================================================================

// 形如 "01 02 ff"。
std::string to_hex_string(const uint8_t* data, std::size_t size);

// 解析 "0102ff" 或 "01 02 ff"，非法输入返回空。
std::vector<uint8_t> from_hex_string(const std::string& text);

}  // namespace adisplay::common
