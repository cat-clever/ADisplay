// ADisplay —— SSDP 报文的构建与解析
//
// 这里放的都是纯函数：输入结构体与字符串，输出字符串或判定结果，
// 不碰 socket、不起线程。单独拎出来是为了能被单元测试直接覆盖 ——
// 一个头字段名写错，真机上的表现就是「手机搜不到设备」，
// 而且要抓包才能定位，成本很高。
//
// 这是内部头文件，不属于公开 API（公开的在 include/adisplay/discovery/ 下）。
// 测试通过额外加一条 include 路径来访问它。
#pragma once

#include <adisplay/discovery/SsdpServer.h>

#include <cstdint>
#include <string>
#include <vector>

namespace adisplay::discovery::ssdp {

// SSDP 的固定组播地址与端口。
inline constexpr const char* kMulticastAddress = "239.255.255.250";
inline constexpr uint16_t kPort = 1900;

// 一条通告对应一个 (NT, USN) 组合。rootdevice、uuid、devicetype
// 以及每个 service type 都要各发一条。
struct NotificationTarget {
    std::string nt;
    std::string usn;
};

// 从 HTTP 风格的报文里取某个头。头名大小写不敏感。
// 找不到返回空串。
std::string header_value(const std::string& message, const std::string& name);

// 去掉首尾空白。
std::string trim(const std::string& text);

// 全部转小写。
std::string to_lower(std::string text);

// RFC 1123 格式的日期，SSDP 的 DATE 头要求这个格式。
// 形如 "Tue, 08 Oct 2026 04:50:00 GMT"。
std::string http_date();

// 按通告内容展开出全部 NT/USN 组合。
std::vector<NotificationTarget> build_targets(const SsdpAdvertisement& advertisement);

// NOTIFY ssdp:alive —— 周期性宣告自己还在。
std::string build_alive_message(const SsdpAdvertisement& advertisement,
                                const NotificationTarget& target);

// NOTIFY ssdp:byebye —— 关闭时发，让手机立刻把设备移出列表。
std::string build_byebye_message(const SsdpAdvertisement& advertisement,
                                 const NotificationTarget& target);

// HTTP/1.1 200 OK —— 回复 M-SEARCH。必须单播回请求方。
std::string build_search_response(const SsdpAdvertisement& advertisement,
                                  const std::string& search_target);

// 判断查询的 ST 是否指向我们。ssdp:all 表示「把所有设备都报一遍」。
bool matches_search_target(const SsdpAdvertisement& advertisement,
                           const std::string& search_target);

// 判断一个报文是不是 M-SEARCH 请求。
bool is_search_request(const std::string& message);

}  // namespace adisplay::discovery::ssdp
