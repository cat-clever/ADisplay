// ADisplay —— SOAP 请求解析与应答构建
//
// DLNA 的控制全靠 SOAP：手机把动作 POST 到 /control，我们用 SOAPAction 头
// 判断是哪个服务的哪个动作，解析参数，执行，再按规范格式回一个信封。
//
// 这里都是纯函数（除了用 tinyxml2 解析 XML），不碰网络，能被单元测试覆盖。
// 应答格式错一个元素名，手机端就表现为「点了没反应」—— 而它不会告诉你
// 是哪里不对，所以这部分必须有测试兜住。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace adisplay::dlna::soap {

// UPnP 规范定义的错误码。手机端会区分对待：
// 401/402 表示请求本身有问题，不会重试；501 表示动作执行失败，可能会重试。
namespace error_code {
inline constexpr int kInvalidAction = 401;
inline constexpr int kInvalidArgs = 402;
inline constexpr int kActionFailed = 501;
inline constexpr int kArgumentValueInvalid = 600;
inline constexpr int kArgumentValueOutOfRange = 601;
}  // namespace error_code

// 一个解析好的动作请求。
struct ActionRequest {
    // 来自 SOAPAction 头，形如
    // "urn:schemas-upnp-org:service:AVTransport:1#Play"。
    std::string service_type;

    // 动作名，形如 "Play"。从信封体里的元素名取，正常情况下与头部一致；
    // 不一致时以信封体为准 —— 部分实现在头里写得不准。
    std::string action_name;

    // 参数名 → 值。参数名不含命名空间前缀。
    std::vector<std::pair<std::string, std::string>> arguments;

    // 取参数。不存在返回 fallback。
    std::string get(const std::string& name, const std::string& fallback = std::string()) const;
};

// 解析 SOAP 请求。
//
// soap_action_header 是 HTTP 头 SOAPAction 的原始值（可能带引号）。
// body 是请求体。失败时返回 false 并把原因写进 out_error。
bool parse_action_request(const std::string& soap_action_header,
                          const std::string& body,
                          ActionRequest* out_request,
                          std::string* out_error);

// 构建成功应答的信封。
// results 是「输出参数名 → 值」的有序列表，顺序应当与 SCPD 里声明的一致 ——
// 部分手机端按位置取值。
std::string build_action_response(const std::string& service_type,
                                  const std::string& action_name,
                                  const std::vector<std::pair<std::string, std::string>>& results);

// 构建错误应答。description 会原样放进 errorDescription，
// 手机端一般直接忽略它，但排障时有用。
std::string build_fault(int error_code, const std::string& description);

// 从 SOAPAction 头里取服务类型（# 之前的部分）。
// 头值可能带双引号，这里一并处理。
std::string extract_service_type(const std::string& soap_action_header);

// 把毫秒数格式化成 UPnP 的时间格式 "H:MM:SS"。负数返回 "0:00:00"。
// 注意 UPnP 用的是 1 位小时 + 2 位分秒，不是 ISO 8601 的 2 位小时。
std::string format_duration(int64_t milliseconds);

// 解析 UPnP 时间格式。失败返回 -1。
int64_t parse_duration(const std::string& text);

// XML 文本转义（与 description 模块共用同样的规则）。
std::string escape_xml(const std::string& text);

}  // namespace adisplay::dlna::soap
