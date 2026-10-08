// 设备名称规则的单元测试（文档 2.4）。
//
// 这些规则直接决定手机端投屏列表里显示什么，边界情况（超长、控制字符、
// 重名）在真机上表现为「设备名乱码」或「搜不到设备」，所以逐条钉住。
#include <adisplay/common/DeviceName.h>

#include "AdTest.h"

using namespace adisplay::common;

AD_TEST(empty_name_invalid, "空名称不合规") {
    AD_CHECK(!validate_device_name("").valid);
    AD_CHECK(!validate_device_name(std::string_view()).valid);
}

AD_TEST(whitespace_only_invalid, "纯空白名称不合规") {
    AD_CHECK(!validate_device_name("   ").valid);
    AD_CHECK(!validate_device_name("\t\n ").valid);
    AD_CHECK(!validate_device_name("\xE3\x80\x80").valid);   // 全角空格 U+3000
}

AD_TEST(single_char_valid, "单个字符合规") {
    const DeviceNameValidation result = validate_device_name("A");
    AD_CHECK(result.valid);
    AD_CHECK_EQ(result.normalized, std::string("A"));
}

AD_TEST(exactly_32_chars_valid, "恰好32个字符合规") {
    const std::string name(32, 'x');
    const DeviceNameValidation result = validate_device_name(name);
    AD_CHECK(result.valid);
    AD_CHECK_EQ(result.normalized, name);
}

AD_TEST(33_chars_invalid, "33个字符不合规") {
    const std::string name(33, 'x');
    const DeviceNameValidation result = validate_device_name(name);
    AD_CHECK(!result.valid);
    AD_CHECK(!result.reason.empty());
}

AD_TEST(chinese_counted_by_codepoint, "中文按码点计数而非字节") {
    // 「客厅电脑」是 4 个字符、12 个字节。若按字节算会误判成 12 个字符，
    // 32 个汉字的名称就会被错误地拒绝。
    AD_CHECK_EQ(utf8_codepoint_count("客厅电脑"), static_cast<std::size_t>(4));

    const DeviceNameValidation result = validate_device_name("客厅电脑");
    AD_CHECK(result.valid);
    AD_CHECK_EQ(result.normalized, std::string("客厅电脑"));
}

AD_TEST(chinese_32_valid_33_invalid, "32个汉字合规_33个不合规") {
    std::string name32;
    for (int i = 0; i < 32; ++i) {
        name32 += "电";   // 每个 3 字节
    }
    AD_CHECK_EQ(name32.size(), static_cast<std::size_t>(96));
    AD_CHECK(validate_device_name(name32).valid);

    const std::string name33 = name32 + "电";
    AD_CHECK(!validate_device_name(name33).valid);
}

AD_TEST(control_chars_filtered, "控制字符被自动过滤") {
    // 文档 2.4：「自动过滤控制字符」。这里的名称过滤后是「客厅电脑」。
    const DeviceNameValidation result = validate_device_name("客\x01厅\x7F电\x1F脑");
    AD_CHECK(result.valid);
    AD_CHECK_EQ(result.normalized, std::string("客厅电脑"));
}

AD_TEST(zero_width_chars_filtered, "零宽字符被过滤") {
    // U+200B 零宽空格会让手机端列表出现看不见的间隙。
    const DeviceNameValidation result = validate_device_name("客\xE2\x80\x8B厅");
    AD_CHECK(result.valid);
    AD_CHECK_EQ(result.normalized, std::string("客厅"));
}

AD_TEST(only_control_chars_invalid, "仅由控制字符组成的名称不合规") {
    AD_CHECK(!validate_device_name("\x01\x02\x03").valid);
}

AD_TEST(leading_trailing_space_trimmed, "首尾空白被裁掉") {
    const DeviceNameValidation result = validate_device_name("  客厅电脑  ");
    AD_CHECK(result.valid);
    AD_CHECK_EQ(result.normalized, std::string("客厅电脑"));
}

AD_TEST(inner_space_preserved, "名称中间的空白保留") {
    const DeviceNameValidation result = validate_device_name("客厅 电脑");
    AD_CHECK(result.valid);
    AD_CHECK_EQ(result.normalized, std::string("客厅 电脑"));
}

AD_TEST(common_symbols_allowed, "常见符号允许") {
    const DeviceNameValidation result = validate_device_name("Living-Room_PC (2)");
    AD_CHECK(result.valid);
    AD_CHECK_EQ(result.normalized, std::string("Living-Room_PC (2)"));
}

AD_TEST(invalid_utf8_is_safe, "非法UTF8不会导致崩溃") {
    // 孤立的续接字节。函数应当把它当作替换字符，而不是越界读。
    const std::string broken = "abc\x80\x80" "def";
    const DeviceNameValidation result = validate_device_name(broken);
    AD_CHECK(result.valid);
    AD_CHECK(!result.normalized.empty());
}

AD_TEST(truncate_by_codepoint, "按码点截断不会切碎多字节字符") {
    const std::string text = "客厅电脑";
    AD_CHECK_EQ(utf8_truncate(text, 2), std::string("客厅"));
    AD_CHECK_EQ(utf8_truncate(text, 4), text);
    AD_CHECK_EQ(utf8_truncate(text, 10), text);   // 超长不补
    AD_CHECK_EQ(utf8_truncate(text, 0), std::string(""));

    // 截断结果必须是合法的 UTF-8 前缀，不能出现半个汉字。
    const std::string truncated = utf8_truncate(text, 3);
    AD_CHECK_EQ(utf8_codepoint_count(truncated), static_cast<std::size_t>(3));
}

AD_TEST(dedupe_no_conflict, "重名时无冲突则原样返回") {
    const std::vector<std::string> taken = {"卧室电脑", "书房电脑"};
    AD_CHECK_EQ(dedupe_device_name("客厅电脑", taken), std::string("客厅电脑"));
}

AD_TEST(dedupe_appends_suffix, "重名时追加数字后缀") {
    const std::vector<std::string> taken = {"客厅电脑"};
    AD_CHECK_EQ(dedupe_device_name("客厅电脑", taken), std::string("客厅电脑 (2)"));
}

AD_TEST(dedupe_increments_suffix, "重名后缀递增到第一个空位") {
    const std::vector<std::string> taken = {"客厅电脑", "客厅电脑 (2)", "客厅电脑 (3)"};
    AD_CHECK_EQ(dedupe_device_name("客厅电脑", taken), std::string("客厅电脑 (4)"));
}

AD_TEST(dedupe_respects_max_length, "重名后缀不会让名称超过32字符") {
    // 32 个汉字 + " (2)" 会超限，必须先截断基名再拼后缀。
    std::string name32;
    for (int i = 0; i < 32; ++i) {
        name32 += "电";
    }
    const std::vector<std::string> taken = {name32};

    const std::string deduped = dedupe_device_name(name32, taken);
    AD_CHECK(utf8_codepoint_count(deduped) <= kDeviceNameMaxChars);
    AD_CHECK(deduped.size() > 4);
    // 尾部应当是 " (2)"
    AD_CHECK_EQ(deduped.substr(deduped.size() - 4), std::string(" (2)"));
    // 结果本身要能通过校验
    AD_CHECK(validate_device_name(deduped).valid);
}

AD_TEST(dedupe_empty_taken_list, "空冲突列表不处理") {
    const std::vector<std::string> taken;
    AD_CHECK_EQ(dedupe_device_name("客厅电脑", taken), std::string("客厅电脑"));
}

AD_TEST(default_device_name_valid, "默认设备名非空且合规") {
    const std::string name = default_device_name();
    AD_CHECK(!name.empty());
    AD_CHECK(validate_device_name(name).valid);
}

AD_TEST(sanitize_is_idempotent, "规整函数幂等") {
    const std::string once = sanitize_device_name("  客\x01厅  ");
    const std::string twice = sanitize_device_name(once);
    AD_CHECK_EQ(once, twice);
}

int main() {
    return adtest::run_all("设备名称测试");
}
