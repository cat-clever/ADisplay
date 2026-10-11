// ADisplay —— 设备名称规则（文档 2.4「设备名称可修改」）
//
// 文档原文约束：
//   * 输入规则：1–32 个字符，支持中文、字母、数字、空格和常见符号；
//     不能为空，自动过滤控制字符。
//   * 重名处理：局域网内出现同名设备时，自动追加数字后缀（如「客厅电脑 (2)」）。
//   * 即时生效：保存后注销并重新注册 mDNS / SSDP，无需重启软件。
//   * 标识不变：只改显示名，deviceid / UUID 与已配对记录不变。
//
// 注意「字符」按 Unicode 码点计，不是字节 —— 中文一个汉字算 1 个字符，
// 否则「客厅电脑」会被误判成 12 个字符。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace adisplay::common {

// 名称长度上下限，单位是 Unicode 码点。
constexpr std::size_t kDeviceNameMinChars = 1;
constexpr std::size_t kDeviceNameMaxChars = 32;

struct DeviceNameValidation {
    bool valid = false;

    // 不合规时的人话说明（中文，UTF-8），界面层可直接弹给用户。
    std::string reason;

    // 过滤掉控制字符、首尾空白之后的结果。
    // valid 为 true 时，应当用这个值，而不是用户原始输入。
    std::string normalized;
};

// 校验并规整设备名称。任何情况下都不会抛异常。
DeviceNameValidation validate_device_name(std::string_view utf8_name);

// 只做功整，不做校验。用于「把非法输入清理一遍」的场景。
std::string sanitize_device_name(std::string_view utf8_name);

// 按 Unicode 码点计数。非法 UTF-8 序列按 1 个码点计，不中断。
std::size_t utf8_codepoint_count(std::string_view utf8_text);

// 按码点数截断到 limit，保证不会把一个多字节字符切成两半。
std::string utf8_truncate(std::string_view utf8_text, std::size_t limit);

// 重名处理：若 name 已在 taken 中出现，依次尝试「name (2)」「name (3)」……
// 直到不冲突。加了后缀仍超出 32 码点时，会先截断基名再拼后缀。
// taken 里的比较是精确匹配（区分大小写，符合手机端列表的显示行为）。
std::string dedupe_device_name(std::string_view name, const std::vector<std::string>& taken);

// 取平台默认设备名：桌面端取主机名，电视端取设备型号（文档 2.4）。
// 失败时返回 "ADisplay"。
std::string default_device_name();

}  // namespace adisplay::common
