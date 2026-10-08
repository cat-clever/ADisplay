// DLNA 设备描述与 SCPD 的生成测试。
//
// 手机发现设备后的第一件事就是拉 description.xml。拉不到、或者 XML 里少了
// 某个必需元素，手机会直接把设备从列表里去掉，而且不给任何有用的提示 ——
// 表现就是「看得到一秒然后消失」或者「压根不出现」。所以逐字段断言。
#include "DlnaDescription.h"

#include "AdTest.h"

#include <string>

using namespace adisplay::dlna;
using namespace adisplay::dlna::description;

namespace {

DlnaConfig make_config() {
    DlnaConfig config;
    config.device_name = "客厅电脑";
    config.uuid = "550e8400-e29b-41d4-a716-446655440000";
    config.manufacturer = "ADisplay";
    config.model_name = "ADisplay Receiver";
    config.model_number = "1";
    config.http_port = 49152;
    return config;
}

// 简单的「包含子串」断言，失败时能把 XML 片段打出来。
void check_contains(const std::string& haystack, const std::string& needle) {
    if (haystack.find(needle) == std::string::npos) {
        AD_CHECK_EQ(haystack.substr(0, 400), std::string("... 未包含: ") + needle);
    }
}

}  // namespace

AD_TEST(xml_escape, "escape_xml 转义五个特殊字符") {
    AD_CHECK_EQ(escape_xml("a&b"), std::string("a&amp;b"));
    AD_CHECK_EQ(escape_xml("<tag>"), std::string("&lt;tag&gt;"));
    AD_CHECK_EQ(escape_xml("\"quoted\""), std::string("&quot;quoted&quot;"));
    AD_CHECK_EQ(escape_xml("it's"), std::string("it&apos;s"));
    AD_CHECK_EQ(escape_xml("普通文本"), std::string("普通文本"));
    AD_CHECK_EQ(escape_xml(""), std::string(""));
}

AD_TEST(设备名含特殊字符不会破坏XML, "设备名里的 & 被正确转义") {
    DlnaConfig config = make_config();
    // 文档 2.4 允许常见符号，& 是合法的设备名组成部分。
    config.device_name = "Tom & Jerry's <PC>";

    const std::string xml = build_device_description(config, "http://192.168.1.20:49152");

    check_contains(xml, "<friendlyName>Tom &amp; Jerry&apos;s &lt;PC&gt;</friendlyName>");
    // 原始的 & 不该出现在 XML 里（除了实体引用），否则整份文档非法。
    AD_CHECK(xml.find("Tom & Jerry") == std::string::npos);
}

AD_TEST(设备描述含必需元素, "description.xml 的必需元素齐全") {
    const DlnaConfig config = make_config();
    const std::string xml = build_device_description(config, "http://192.168.1.20:49152");

    check_contains(xml, "<?xml version=\"1.0\" encoding=\"utf-8\"?>");
    // root 上现在同时声明了 DLNA 的命名空间（给 X_DLNADOC 用）
    check_contains(xml, "urn:schemas-upnp-org:device-1-0");
    check_contains(xml, "xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\"");
    check_contains(xml, "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>");
    check_contains(xml, "<friendlyName>客厅电脑</friendlyName>");
    check_contains(xml, "<specVersion><major>1</major><minor>0</minor></specVersion>");
    // UDN 的 "uuid:" 前缀不能少，少了手机认不出这是 UPnP 设备。
    check_contains(xml, "<UDN>uuid:550e8400-e29b-41d4-a716-446655440000</UDN>");
}

AD_TEST(设备描述含DLNA认证标记, "设备描述带 X_DLNADOC 认证标记") {
    const DlnaConfig config = make_config();
    const std::string xml = build_device_description(config, "http://192.168.1.20:49152");

    // 少了这个标记，部分国产视频 App 会认为这不是合格的 DLNA 设备，
    // 直接不列出来 —— 表现是「B 站能搜到、另一个 App 搜不到」。
    check_contains(xml, "<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>");
    check_contains(xml, "xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\"");
}

AD_TEST(设备描述声明三个必需服务, "三个 DLNA 必需服务都声明了") {
    const DlnaConfig config = make_config();
    const std::string xml = build_device_description(config, "http://192.168.1.20:49152");

    check_contains(xml, kAvTransportType);
    check_contains(xml, kRenderingControlType);
    check_contains(xml, kConnectionManagerType);

    check_contains(xml, kAvTransportId);
    check_contains(xml, kRenderingControlId);
    check_contains(xml, kConnectionManagerId);

    // 每个服务都要有这三个 URL，少一个手机就调不动那个服务。
    AD_CHECK(xml.find("<controlURL>/control</controlURL>") != std::string::npos);
    AD_CHECK(xml.find("<eventSubURL>/event</eventSubURL>") != std::string::npos);
    AD_CHECK(xml.find("<SCPDURL>") != std::string::npos);
}

AD_TEST(AVTransport的SCPD含全部动作, "AVTransport 的 SCPD 含各必需动作") {
    const std::string scpd = build_service_description(kAvTransportType);
    AD_CHECK(!scpd.empty());

    check_contains(scpd, "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">");
    for (const char* action : {"SetAVTransportURI", "GetMediaInfo", "GetTransportInfo",
                               "GetPositionInfo", "Play", "Pause", "Stop", "Seek"}) {
        check_contains(scpd, std::string("<name>") + action + "</name>");
    }
    // TransportState 必须声明 allowedValueList，手机会据此判断设备能不能暂停。
    check_contains(scpd, "<allowedValue>PAUSED_PLAYBACK</allowedValue>");
}

AD_TEST(RenderingControl的SCPD, "RenderingControl 的 SCPD 含音量动作与范围") {
    const std::string scpd = build_service_description(kRenderingControlType);
    AD_CHECK(!scpd.empty());

    for (const char* action : {"GetVolume", "SetVolume", "GetMute", "SetMute"}) {
        check_contains(scpd, std::string("<name>") + action + "</name>");
    }
    check_contains(scpd, "<allowedValueRange><minimum>0</minimum><maximum>100</maximum>");
}

AD_TEST(ConnectionManager的SCPD, "ConnectionManager 的 SCPD 含 GetProtocolInfo") {
    const std::string scpd = build_service_description(kConnectionManagerType);
    AD_CHECK(!scpd.empty());
    check_contains(scpd, "<name>GetProtocolInfo</name>");
}

AD_TEST(未知服务返回空, "未知服务类型的 SCPD 为空") {
    AD_CHECK_EQ(build_service_description("urn:schemas-upnp-org:service:Nope:1"), std::string(""));
    AD_CHECK_EQ(build_service_description(""), std::string(""));
}

AD_TEST(从元数据取标题, "extract_title_from_metadata 取 dc:title") {
    const std::string metadata =
        "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<item><dc:title>星际穿越</dc:title></item></DIDL-Lite>";
    AD_CHECK_EQ(extract_title_from_metadata(metadata), std::string("星际穿越"));
}

AD_TEST(标题里的实体被还原, "标题里的 XML 实体被还原") {
    // &amp; 必须最后还原，否则 "&amp;lt;" 会被错误地解成 "<"。
    const std::string metadata = "<dc:title>Tom &amp; Jerry &lt;HD&gt;</dc:title>";
    AD_CHECK_EQ(extract_title_from_metadata(metadata), std::string("Tom & Jerry <HD>"));
}

AD_TEST(元数据缺标题返回空, "元数据里没有标题时返回空串") {
    AD_CHECK_EQ(extract_title_from_metadata(""), std::string(""));
    AD_CHECK_EQ(extract_title_from_metadata("<DIDL-Lite></DIDL-Lite>"), std::string(""));
    AD_CHECK_EQ(extract_title_from_metadata("<dc:title></dc:title>"), std::string(""));
}

int main() {
    return adtest::run_all("DLNA 设备描述测试");
}
