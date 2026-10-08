// ADisplay —— DLNA 设备描述与服务描述的 XML 生成
//
// 手机发现设备后的第一件事就是 GET /description.xml。拉不到、或者 XML 里
// 少了某个必需元素，手机会直接把设备从列表里去掉 —— 而且不会给出任何
// 有用的提示，表现就是「看得到一秒然后消失」或者「压根不出现」。
//
// 所以这里都是纯函数：给定配置，产出确定的 XML。能被单元测试逐字段断言。
#pragma once

#include <adisplay/dlna/DlnaRenderer.h>

#include <string>

namespace adisplay::dlna::description {

// 三个必需服务的类型与 serviceId。UPnP 规范要求 serviceId 形如
// urn:upnp-org:serviceId:AVTransport，写成别的手机端会认不出来。
inline constexpr const char* kAvTransportType =
    "urn:schemas-upnp-org:service:AVTransport:1";
inline constexpr const char* kRenderingControlType =
    "urn:schemas-upnp-org:service:RenderingControl:1";
inline constexpr const char* kConnectionManagerType =
    "urn:schemas-upnp-org:service:ConnectionManager:1";

inline constexpr const char* kAvTransportId = "urn:upnp-org:serviceId:AVTransport";
inline constexpr const char* kRenderingControlId = "urn:upnp-org:serviceId:RenderingControl";
inline constexpr const char* kConnectionManagerId = "urn:upnp-org:serviceId:ConnectionManager";

// 生成设备描述 XML（根元素 <root>），包含 deviceType、friendlyName、
// UDN、三个 service 的声明以及每个服务的 SCPDURL / controlURL / eventSubURL。
//
// base_url 形如 "http://192.168.1.20:49152"，用来拼各服务的 URL。
std::string build_device_description(const DlnaConfig& config, const std::string& base_url);

// 生成某个服务的 SCPD（Service Control Protocol Description）。
// 手机在调用动作前会先拉它，拿不到可能会拒绝发 SOAP 请求。
//
// service_type 传上面那三个常量之一。未知类型返回空串。
std::string build_service_description(const std::string& service_type);

// XML 文本转义。设备名里可能有 & < > 等字符（文档 2.4 允许常见符号），
// 不转义的话设备描述会直接变成非法 XML。
std::string escape_xml(const std::string& text);

// 从 DIDL-Lite 元数据里取标题，供界面显示「正在播放：xxx」。
// 取不到返回空串。
std::string extract_title_from_metadata(const std::string& metadata);

}  // namespace adisplay::dlna::description
