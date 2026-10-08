// SSDP 报文的构建与解析测试（文档第 10 节要求覆盖核心解析逻辑）。
//
// 这层逻辑出错的表现是「手机搜不到设备」，而且要看抓包才能定位 ——
// 一个头字段名拼错、LOCATION 少个端口、USN 格式不对，全都不会报错，
// 只是设备安静地不出现。所以逐条钉死。
#include "SsdpMessage.h"

#include "AdTest.h"

#include <string>

using namespace adisplay::discovery;
using namespace adisplay::discovery::ssdp;

namespace {

// 一份有代表性的 MediaRenderer 通告。
SsdpAdvertisement make_advertisement() {
    SsdpAdvertisement advertisement;
    advertisement.udn = "uuid:550e8400-e29b-41d4-a716-446655440000";
    advertisement.device_type = "urn:schemas-upnp-org:device:MediaRenderer:1";
    advertisement.location = "http://192.168.1.20:49152/description.xml";
    advertisement.service_types = {
        "urn:schemas-upnp-org:service:AVTransport:1",
        "urn:schemas-upnp-org:service:RenderingControl:1",
        "urn:schemas-upnp-org:service:ConnectionManager:1",
    };
    advertisement.max_age_seconds = 1800;
    advertisement.server_header = "UPnP/1.0 ADisplay/0.1.0";
    return advertisement;
}

// 报文的第一个换行前面的部分。
std::string first_line(const std::string& message) {
    const std::size_t end = message.find("\r\n");
    return end == std::string::npos ? message : message.substr(0, end);
}

}  // namespace

AD_TEST(trim_去掉首尾空白, "trim 去掉首尾空白") {
    AD_CHECK_EQ(trim("  hello  "), std::string("hello"));
    AD_CHECK_EQ(trim("\t\r\n x \r\n"), std::string("x"));
    AD_CHECK_EQ(trim(""), std::string(""));
    AD_CHECK_EQ(trim("   "), std::string(""));
    AD_CHECK_EQ(trim("abc"), std::string("abc"));
}

AD_TEST(to_lower_转小写, "to_lower 转小写") {
    AD_CHECK_EQ(to_lower("M-SEARCH"), std::string("m-search"));
    AD_CHECK_EQ(to_lower("St"), std::string("st"));
    AD_CHECK_EQ(to_lower(""), std::string(""));
}

AD_TEST(header_大小写不敏感, "header_value 头名大小写不敏感") {
    const std::string message =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: ssdp:all\r\n"
        "\r\n";

    AD_CHECK_EQ(header_value(message, "ST"), std::string("ssdp:all"));
    AD_CHECK_EQ(header_value(message, "st"), std::string("ssdp:all"));
    AD_CHECK_EQ(header_value(message, "St"), std::string("ssdp:all"));
    AD_CHECK_EQ(header_value(message, "MX"), std::string("1"));
    AD_CHECK_EQ(header_value(message, "HOST"), std::string("239.255.255.250:1900"));
}

AD_TEST(header_缺失返回空, "header_value 缺失时返回空串") {
    const std::string message = "M-SEARCH * HTTP/1.1\r\nST: ssdp:all\r\n\r\n";
    AD_CHECK_EQ(header_value(message, "MX"), std::string(""));
    AD_CHECK_EQ(header_value(message, "LOCATION"), std::string(""));
}

AD_TEST(header_跳过请求行, "header_value 不会把请求行当成头") {
    // 请求行是 "ST: ..." 这种形式的话（虽然协议上不会），也不该被当成头。
    const std::string message = "M-SEARCH * HTTP/1.1\r\nST: real-value\r\n\r\n";
    AD_CHECK_EQ(header_value(message, "M-SEARCH"), std::string(""));
    AD_CHECK_EQ(header_value(message, "ST"), std::string("real-value"));
}

AD_TEST(header_容忍无CR的行尾, "header_value 容忍只有 LF 的行尾") {
    const std::string message = "M-SEARCH * HTTP/1.1\nST: ssdp:all\n\n";
    AD_CHECK_EQ(header_value(message, "ST"), std::string("ssdp:all"));
}

AD_TEST(header_取冒号后第一个值, "header_value 处理值里含冒号的情况") {
    const std::string message =
        "HTTP/1.1 200 OK\r\n"
        "LOCATION: http://192.168.1.20:49152/description.xml\r\n"
        "\r\n";
    AD_CHECK_EQ(header_value(message, "LOCATION"),
                std::string("http://192.168.1.20:49152/description.xml"));
}

AD_TEST(识别搜索请求, "is_search_request 只认 M-SEARCH") {
    AD_CHECK(is_search_request("M-SEARCH * HTTP/1.1\r\n\r\n"));
    AD_CHECK(!is_search_request("NOTIFY * HTTP/1.1\r\n\r\n"));
    AD_CHECK(!is_search_request("HTTP/1.1 200 OK\r\n\r\n"));
    AD_CHECK(!is_search_request(""));
}

AD_TEST(展开通告目标, "build_targets 展开 rootdevice/uuid/设备类型/各服务") {
    const SsdpAdvertisement advertisement = make_advertisement();
    const std::vector<NotificationTarget> targets = build_targets(advertisement);

    // rootdevice + udn + devicetype + 3 个服务 = 6
    AD_CHECK_EQ(targets.size(), static_cast<std::size_t>(6));

    AD_CHECK_EQ(targets[0].nt, std::string("upnp:rootdevice"));
    AD_CHECK_EQ(targets[0].usn, advertisement.udn + "::upnp:rootdevice");

    AD_CHECK_EQ(targets[1].nt, advertisement.udn);
    AD_CHECK_EQ(targets[2].nt, advertisement.device_type);
    AD_CHECK_EQ(targets[3].nt, advertisement.service_types[0]);
    AD_CHECK_EQ(targets[5].nt, advertisement.service_types[2]);

    // 每个 USN 都必须是 "udn::nt" 的形式，UPnP 靠它对上号。
    for (const NotificationTarget& target : targets) {
        AD_CHECK_EQ(target.usn, advertisement.udn + "::" + target.nt);
    }
}

AD_TEST(alive报文含必需的头, "NOTIFY alive 含 LOCATION/CACHE-CONTROL/NTS") {
    const SsdpAdvertisement advertisement = make_advertisement();
    const std::vector<NotificationTarget> targets = build_targets(advertisement);
    const std::string message = build_alive_message(advertisement, targets[0]);

    AD_CHECK_EQ(first_line(message), std::string("NOTIFY * HTTP/1.1"));
    AD_CHECK_EQ(header_value(message, "HOST"), std::string("239.255.255.250:1900"));
    AD_CHECK_EQ(header_value(message, "NTS"), std::string("ssdp:alive"));
    AD_CHECK_EQ(header_value(message, "NT"), std::string("upnp:rootdevice"));
    AD_CHECK_EQ(header_value(message, "LOCATION"), advertisement.location);
    AD_CHECK_EQ(header_value(message, "CACHE-CONTROL"), std::string("max-age=1800"));
    AD_CHECK_EQ(header_value(message, "SERVER"), advertisement.server_header);
    AD_CHECK_EQ(header_value(message, "USN"), targets[0].usn);
}

AD_TEST(byebye报文不带位置, "NOTIFY byebye 不带 LOCATION 与 CACHE-CONTROL") {
    const SsdpAdvertisement advertisement = make_advertisement();
    const std::vector<NotificationTarget> targets = build_targets(advertisement);
    const std::string message = build_byebye_message(advertisement, targets[1]);

    AD_CHECK_EQ(first_line(message), std::string("NOTIFY * HTTP/1.1"));
    AD_CHECK_EQ(header_value(message, "NTS"), std::string("ssdp:byebye"));
    AD_CHECK_EQ(header_value(message, "USN"), targets[1].usn);

    // 规范里 byebye 只用于注销，带 LOCATION 会让部分实现重新把它加回列表。
    AD_CHECK_EQ(header_value(message, "LOCATION"), std::string(""));
    AD_CHECK_EQ(header_value(message, "CACHE-CONTROL"), std::string(""));
}

AD_TEST(搜索响应的字段, "M-SEARCH 响应的字段齐全") {
    const SsdpAdvertisement advertisement = make_advertisement();
    const std::string message = build_search_response(advertisement, "ssdp:all");

    AD_CHECK_EQ(first_line(message), std::string("HTTP/1.1 200 OK"));
    AD_CHECK_EQ(header_value(message, "ST"), std::string("ssdp:all"));
    AD_CHECK_EQ(header_value(message, "LOCATION"), advertisement.location);
    AD_CHECK_EQ(header_value(message, "CACHE-CONTROL"), std::string("max-age=1800"));
    AD_CHECK_EQ(header_value(message, "SERVER"), advertisement.server_header);
    AD_CHECK_EQ(header_value(message, "USN"),
                advertisement.udn + "::ssdp:all");

    // EXT 头必须存在且值为空 —— 这是 UPnP 规范要求的。
    AD_CHECK(message.find("EXT:\r\n") != std::string::npos);
    AD_CHECK(!header_value(message, "DATE").empty());
}

AD_TEST(匹配搜索目标, "matches_search_target 的各类查询") {
    const SsdpAdvertisement advertisement = make_advertisement();

    // ssdp:all 表示「把所有设备都报一遍」。
    AD_CHECK(matches_search_target(advertisement, "ssdp:all"));
    AD_CHECK(matches_search_target(advertisement, "upnp:rootdevice"));
    AD_CHECK(matches_search_target(advertisement, advertisement.udn));
    AD_CHECK(matches_search_target(advertisement, advertisement.device_type));
    AD_CHECK(matches_search_target(advertisement, advertisement.service_types[0]));

    // 别人的设备不该被我们应答 —— 应答了局域网里会出现重复设备。
    AD_CHECK(!matches_search_target(advertisement, "uuid:00000000-0000-0000-0000-000000000000"));
    AD_CHECK(!matches_search_target(advertisement, "urn:schemas-upnp-org:service:ContentDirectory:1"));
    AD_CHECK(!matches_search_target(advertisement, ""));
    AD_CHECK(!matches_search_target(advertisement, "SSDP:ALL"));   // 大小写敏感，规范如此
}

AD_TEST(报文以空行结尾, "报文以 CRLF CRLF 结尾") {
    const SsdpAdvertisement advertisement = make_advertisement();
    const std::vector<NotificationTarget> targets = build_targets(advertisement);

    const std::string alive = build_alive_message(advertisement, targets[0]);
    const std::string response = build_search_response(advertisement, "ssdp:all");

    AD_CHECK(alive.size() >= 4 && alive.compare(alive.size() - 4, 4, "\r\n\r\n") == 0);
    AD_CHECK(response.size() >= 4 && response.compare(response.size() - 4, 4, "\r\n\r\n") == 0);
}

int main() {
    return adtest::run_all("SSDP 报文测试");
}
