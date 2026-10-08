// 字节序与位读取器的单元测试。
//
// 文档 2.5 要求「显式按字节序读写协议包头」，这些函数是 AirPlay 镜像包头
// 与 DLNA SOAP 解析的共同地基，错了会在真机上表现为花屏或连不上，
// 且极难定位 —— 所以这里要把边界情况钉死。
#include <adisplay/common/ByteOrder.h>

#include "AdTest.h"

using namespace adisplay::common;

AD_TEST(read_big_endian, "读取大端整数") {
    const uint8_t data[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};

    AD_CHECK_EQ(read_be16(data), 0x1234u);
    AD_CHECK_EQ(read_be24(data), 0x123456u);
    AD_CHECK_EQ(read_be32(data), 0x12345678u);
    AD_CHECK_EQ(read_be64(data), 0x123456789ABCDEF0ull);
}

AD_TEST(read_little_endian, "读取小端整数") {
    const uint8_t data[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};

    AD_CHECK_EQ(read_le16(data), 0x3412u);
    AD_CHECK_EQ(read_le24(data), 0x563412u);
    AD_CHECK_EQ(read_le32(data), 0x78563412u);
    AD_CHECK_EQ(read_le64(data), 0xF0DEBC9A78563412ull);
}

AD_TEST(write_read_back_big_endian, "写入后能读回_大端") {
    uint8_t buffer[8] = {0};
    write_be16(buffer, 0xABCDu);
    write_be32(buffer + 2, 0xDEADBEEFu);
    write_be16(buffer + 6, 0x1234u);

    AD_CHECK_EQ(read_be16(buffer), 0xABCDu);
    AD_CHECK_EQ(read_be32(buffer + 2), 0xDEADBEEFu);
    AD_CHECK_EQ(read_be16(buffer + 6), 0x1234u);
}

AD_TEST(write_read_back_little_endian, "写入后能读回_小端") {
    uint8_t buffer[8] = {0};
    write_le16(buffer, 0xABCDu);
    write_le32(buffer + 2, 0xDEADBEEFu);
    write_le16(buffer + 6, 0x1234u);

    AD_CHECK_EQ(read_le16(buffer), 0xABCDu);
    AD_CHECK_EQ(read_le32(buffer + 2), 0xDEADBEEFu);
    AD_CHECK_EQ(read_le16(buffer + 6), 0x1234u);
}

AD_TEST(big_endian_matches_network_order, "大端写出的字节顺序符合网络序") {
    uint8_t buffer[4] = {0};
    write_be32(buffer, 0x01020304u);

    AD_CHECK_EQ(static_cast<int>(buffer[0]), 0x01);
    AD_CHECK_EQ(static_cast<int>(buffer[1]), 0x02);
    AD_CHECK_EQ(static_cast<int>(buffer[2]), 0x03);
    AD_CHECK_EQ(static_cast<int>(buffer[3]), 0x04);
}

AD_TEST(append_helpers_concatenate, "追加函数按顺序拼接") {
    std::vector<uint8_t> out;
    append_be16(out, 0x0102u);
    append_be32(out, 0x03040506u);
    append_le16(out, 0x0807u);

    AD_CHECK_EQ(out.size(), static_cast<std::size_t>(8));
    AD_CHECK_EQ(static_cast<int>(out[0]), 0x01);
    AD_CHECK_EQ(static_cast<int>(out[1]), 0x02);
    AD_CHECK_EQ(static_cast<int>(out[2]), 0x03);
    AD_CHECK_EQ(static_cast<int>(out[5]), 0x06);
    AD_CHECK_EQ(static_cast<int>(out[6]), 0x07);   // 小端：低位在前
    AD_CHECK_EQ(static_cast<int>(out[7]), 0x08);
}

AD_TEST(bit_reader_msb_first, "位读取器_MSB优先") {
    // 0xB4 = 1011 0100
    const uint8_t data[] = {0xB4, 0x2D};

    BitReader reader(data, sizeof(data));

    AD_CHECK_EQ(reader.read_msb(1), 1u);
    AD_CHECK_EQ(reader.read_msb(1), 0u);
    AD_CHECK_EQ(reader.read_msb(2), 3u);   // 11
    AD_CHECK_EQ(reader.read_msb(4), 4u);   // 0100
    AD_CHECK_EQ(reader.read_msb(4), 2u);   // 0010 (0x2D 的高 4 位)
    AD_CHECK_EQ(reader.read_msb(4), 13u);  // 1101
    AD_CHECK(!reader.failed());
    AD_CHECK_EQ(reader.bits_left(), static_cast<std::size_t>(0));
}

AD_TEST(bit_reader_lsb_first, "位读取器_LSB优先") {
    // 0xB4 = 1011 0100，LSB 优先读出来是 0010 1101
    const uint8_t data[] = {0xB4};

    BitReader reader(data, sizeof(data));

    AD_CHECK_EQ(reader.read_lsb(4), 4u);    // 低 4 位是 0100 -> 0x4
    AD_CHECK_EQ(reader.read_lsb(4), 11u);   // 高 4 位是 1011 -> 0xB
    AD_CHECK(!reader.failed());
}

AD_TEST(bit_reader_reads_full_64_bits, "位读取器_读满64位") {
    const uint8_t data[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    BitReader reader(data, sizeof(data));
    AD_CHECK_EQ(reader.read_msb(64), 0xFFFFFFFFFFFFFFFFull);
    AD_CHECK(!reader.failed());
}

AD_TEST(bit_reader_out_of_range_sets_failed, "位读取器_越界置失败标志且不崩") {
    const uint8_t data[] = {0xAA};
    BitReader reader(data, sizeof(data));

    AD_CHECK_EQ(reader.read_msb(4), 0xAu);
    reader.read_msb(8);              // 只剩 4 位，越界
    AD_CHECK(reader.failed());
    AD_CHECK_EQ(reader.bits_left(), static_cast<std::size_t>(0));
}

AD_TEST(bit_reader_align_to_byte, "位读取器_对齐到字节") {
    // 镜像包头里位字段之后可能跟裸字节，需要能跳过补位。
    const uint8_t data[] = {0xAB, 0xCD};
    BitReader reader(data, sizeof(data));

    reader.read_msb(3);
    reader.align_to_byte();
    AD_CHECK_EQ(reader.bit_position(), static_cast<std::size_t>(8));
    AD_CHECK_EQ(reader.read_msb(8), 0xCDu);
}

AD_TEST(bit_writer_roundtrip, "位写入器_与读取器往返一致") {
    std::vector<uint8_t> out;
    BitWriter writer(out);
    writer.write_msb(0x5u, 3);    // 101
    writer.write_msb(0x2u, 5);    // 00010  -> 凑满一字节 10100010 = 0xA2
    writer.write_msb(0xFFu, 8);

    AD_CHECK_EQ(out.size(), static_cast<std::size_t>(2));
    AD_CHECK_EQ(static_cast<int>(out[0]), 0xA2);
    AD_CHECK_EQ(static_cast<int>(out[1]), 0xFF);

    BitReader reader(out.data(), out.size());
    AD_CHECK_EQ(reader.read_msb(3), 0x5u);
    AD_CHECK_EQ(reader.read_msb(5), 0x2u);
    AD_CHECK_EQ(reader.read_msb(8), 0xFFu);
}

AD_TEST(hex_string_roundtrip, "十六进制字符串往返") {
    const uint8_t data[] = {0x00, 0x0F, 0xA5, 0xFF};
    const std::string text = to_hex_string(data, sizeof(data));
    AD_CHECK_EQ(text, std::string("00 0f a5 ff"));

    const std::vector<uint8_t> parsed = from_hex_string(text);
    AD_CHECK_EQ(parsed.size(), static_cast<std::size_t>(4));
    AD_CHECK_EQ(static_cast<int>(parsed[0]), 0x00);
    AD_CHECK_EQ(static_cast<int>(parsed[1]), 0x0F);
    AD_CHECK_EQ(static_cast<int>(parsed[2]), 0xA5);
    AD_CHECK_EQ(static_cast<int>(parsed[3]), 0xFF);
}

AD_TEST(hex_parse_tolerates_separators, "十六进制解析_容忍常见分隔符") {
    const std::vector<uint8_t> colon = from_hex_string("AA:BB:CC");
    AD_CHECK_EQ(colon.size(), static_cast<std::size_t>(3));
    AD_CHECK_EQ(static_cast<int>(colon[0]), 0xAA);

    const std::vector<uint8_t> compact = from_hex_string("0102ff");
    AD_CHECK_EQ(compact.size(), static_cast<std::size_t>(3));
    AD_CHECK_EQ(static_cast<int>(compact[2]), 0xFF);

    const std::vector<uint8_t> upper = from_hex_string("DEADBEEF");
    AD_CHECK_EQ(upper.size(), static_cast<std::size_t>(4));
    AD_CHECK_EQ(static_cast<int>(upper[0]), 0xDE);
}

AD_TEST(hex_parse_rejects_invalid, "十六进制解析_非法输入返回空") {
    AD_CHECK_EQ(from_hex_string("zz").size(), static_cast<std::size_t>(0));
    AD_CHECK_EQ(from_hex_string("abc").size(), static_cast<std::size_t>(0));   // 落单半字节
    AD_CHECK_EQ(from_hex_string("01g2").size(), static_cast<std::size_t>(0));
}

AD_TEST(empty_input_is_safe, "空输入不崩") {
    AD_CHECK_EQ(to_hex_string(nullptr, 0), std::string(""));
    AD_CHECK_EQ(from_hex_string("").size(), static_cast<std::size_t>(0));
}

int main() {
    return adtest::run_all("字节序测试");
}
