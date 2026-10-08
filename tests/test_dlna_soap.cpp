// SOAP 请求解析与应答构建的测试。
//
// 应答格式错一个元素名，手机端就表现为「点了没反应」—— 而它不会告诉你
// 是哪里不对。所以要把解析的边界情况和信封结构都钉住。
#include "DlnaSoap.h"

#include "AdTest.h"

#include <string>

using namespace adisplay::dlna::soap;

namespace {

std::string wrap_envelope(const std::string& action) {
    return "<?xml version=\"1.0\"?>"
           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
           "<s:Body>" + action + "</s:Body></s:Envelope>";
}

}  // namespace

AD_TEST(解析基本动作, "解析 Play 动作") {
    const std::string body = wrap_envelope(
        "<u:Play xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID><Speed>1</Speed></u:Play>");

    ActionRequest request;
    std::string error;
    AD_CHECK(parse_action_request(
        "\"urn:schemas-upnp-org:service:AVTransport:1#Play\"", body, &request, &error));
    AD_CHECK_EQ(request.action_name, std::string("Play"));
    AD_CHECK_EQ(request.service_type, std::string("urn:schemas-upnp-org:service:AVTransport:1"));
    AD_CHECK_EQ(request.get("InstanceID"), std::string("0"));
    AD_CHECK_EQ(request.get("Speed"), std::string("1"));
}

AD_TEST(解析带URI的动作, "解析 SetAVTransportURI") {
    const std::string body = wrap_envelope(
        "<u:SetAVTransportURI xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID>"
        "<CurrentURI>http://192.168.1.30:8080/video.mp4</CurrentURI>"
        "<CurrentURIMetaData>&lt;DIDL-Lite&gt;&lt;dc:title&gt;片子&lt;/dc:title&gt;&lt;/DIDL-Lite&gt;</CurrentURIMetaData>"
        "</u:SetAVTransportURI>");

    ActionRequest request;
    std::string error;
    AD_CHECK(parse_action_request("", body, &request, &error));

    AD_CHECK_EQ(request.get("CurrentURI"), std::string("http://192.168.1.30:8080/video.mp4"));
    // tinyxml2 会自动解码实体，拿到的是原始 XML 文本。
    AD_CHECK_EQ(request.get("CurrentURIMetaData"),
                std::string("<DIDL-Lite><dc:title>片子</dc:title></DIDL-Lite>"));
}

AD_TEST(动作名以信封体为准, "SOAPAction 头不准时以信封体为准") {
    // 有些实现在头里写得不准确，甚至干脆不写。信封体才是可靠的。
    const std::string body = wrap_envelope(
        "<u:Play xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"><InstanceID>0</InstanceID></u:Play>");

    ActionRequest request;
    std::string error;
    AD_CHECK(parse_action_request("\"urn:schemas-upnp-org:service:AVTransport:1#Pause\"",
                                  body, &request, &error));
    AD_CHECK_EQ(request.action_name, std::string("Play"));
}

AD_TEST(剥掉命名空间前缀, "元素名的命名空间前缀被剥掉") {
    // 各家手机用的前缀不一样，u: / ns0: / m: 都见过。
    const std::string body = wrap_envelope(
        "<ns0:GetVolume xmlns:ns0=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
        "<ns0:InstanceID>0</ns0:InstanceID><ns0:Channel>Master</ns0:Channel>"
        "</ns0:GetVolume>");

    ActionRequest request;
    std::string error;
    AD_CHECK(parse_action_request("", body, &request, &error));
    AD_CHECK_EQ(request.action_name, std::string("GetVolume"));
    AD_CHECK_EQ(request.get("InstanceID"), std::string("0"));
    AD_CHECK_EQ(request.get("Channel"), std::string("Master"));
}

AD_TEST(取服务类型, "extract_service_type 处理引号与井号") {
    AD_CHECK_EQ(extract_service_type("\"urn:schemas-upnp-org:service:AVTransport:1#Play\""),
                std::string("urn:schemas-upnp-org:service:AVTransport:1"));
    AD_CHECK_EQ(extract_service_type("urn:schemas-upnp-org:service:RenderingControl:1#SetVolume"),
                std::string("urn:schemas-upnp-org:service:RenderingControl:1"));
    // 没有井号时整个值就是服务类型。
    AD_CHECK_EQ(extract_service_type("urn:foo:bar"), std::string("urn:foo:bar"));
}

AD_TEST(解析失败的各种情况, "非法 SOAP 请求被拒绝") {
    ActionRequest request;
    std::string error;

    AD_CHECK(!parse_action_request("", "这不是 XML", &request, &error));
    AD_CHECK(!error.empty());

    AD_CHECK(!parse_action_request("", wrap_envelope(""), &request, &error));
    AD_CHECK(!error.empty());

    // 缺 Envelope
    AD_CHECK(!parse_action_request("", "<foo><s:Body/></foo>", &request, &error));

    // 缺 Body
    AD_CHECK(!parse_action_request("",
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Header/></s:Envelope>",
        &request, &error));
}

AD_TEST(取不存在的参数返回默认值, "ActionRequest::get 的默认值") {
    ActionRequest request;
    request.arguments = {{"InstanceID", "0"}};
    AD_CHECK_EQ(request.get("InstanceID"), std::string("0"));
    AD_CHECK_EQ(request.get("Missing"), std::string(""));
    AD_CHECK_EQ(request.get("Missing", "fallback"), std::string("fallback"));
}

AD_TEST(构建成功应答, "应答信封的结构") {
    const std::string response = build_action_response(
        "urn:schemas-upnp-org:service:AVTransport:1", "GetTransportInfo",
        {{"CurrentTransportState", "PLAYING"}, {"CurrentTransportStatus", "OK"}, {"CurrentSpeed", "1"}});

    AD_CHECK(response.find("<?xml version=\"1.0\" encoding=\"utf-8\"?>") != std::string::npos);
    // SOAP 信封的命名空间必须一字不差。
    AD_CHECK(response.find("<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"") != std::string::npos);
    AD_CHECK(response.find("s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"") != std::string::npos);
    AD_CHECK(response.find("<u:GetTransportInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">") != std::string::npos);
    AD_CHECK(response.find("<CurrentTransportState>PLAYING</CurrentTransportState>") != std::string::npos);
    AD_CHECK(response.find("</u:GetTransportInfoResponse>") != std::string::npos);
}

AD_TEST(应答里的值被转义, "应答参数值里的特殊字符被转义") {
    const std::string response = build_action_response(
        "urn:schemas-upnp-org:service:AVTransport:1", "GetMediaInfo",
        {{"CurrentURI", "http://x/a?b=1&c=2"}});

    // 未转义的 & 会让整份 XML 非法，手机端解析失败。
    AD_CHECK(response.find("http://x/a?b=1&amp;c=2") != std::string::npos);
    AD_CHECK(response.find("b=1&c=2") == std::string::npos);
}

AD_TEST(构建错误应答, "Fault 信封带 UPnPError") {
    const std::string fault = build_fault(error_code::kInvalidAction, "不支持的动作");

    AD_CHECK(fault.find("<s:Fault>") != std::string::npos);
    AD_CHECK(fault.find("<faultcode>s:Client</faultcode>") != std::string::npos);
    AD_CHECK(fault.find("<faultstring>UPnPError</faultstring>") != std::string::npos);
    AD_CHECK(fault.find("xmlns=\"urn:schemas-upnp-org:control-1-0\"") != std::string::npos);
    AD_CHECK(fault.find("<errorCode>401</errorCode>") != std::string::npos);
    AD_CHECK(fault.find("<errorDescription>不支持的动作</errorDescription>") != std::string::npos);
}

AD_TEST(时间格式化, "format_duration 用 UPnP 的 H:MM:SS") {
    // 注意是 1 位小时，不是 ISO 8601 的 2 位 —— 写错进度条会跳。
    AD_CHECK_EQ(format_duration(0), std::string("0:00:00"));
    AD_CHECK_EQ(format_duration(1000), std::string("0:00:01"));
    AD_CHECK_EQ(format_duration(65 * 1000), std::string("0:01:05"));
    AD_CHECK_EQ(format_duration(3661 * 1000), std::string("1:01:01"));
    AD_CHECK_EQ(format_duration(3600 * 1000 * 10), std::string("10:00:00"));
    // 负数当作 0 处理，不能回一个带负号的怪字符串。
    AD_CHECK_EQ(format_duration(-1), std::string("0:00:00"));
}

AD_TEST(时间解析, "parse_duration 解析 H:MM:SS 与带毫秒的形式") {
    AD_CHECK_EQ(parse_duration("0:00:00"), static_cast<int64_t>(0));
    AD_CHECK_EQ(parse_duration("0:01:05"), static_cast<int64_t>(65000));
    AD_CHECK_EQ(parse_duration("1:01:01"), static_cast<int64_t>(3661000));
    AD_CHECK_EQ(parse_duration("0:00:30.500"), static_cast<int64_t>(30500));
    AD_CHECK_EQ(parse_duration("10:00:00"), static_cast<int64_t>(36000000));
}

AD_TEST(时间解析失败, "parse_duration 拒绝非法输入") {
    AD_CHECK_EQ(parse_duration(""), static_cast<int64_t>(-1));
    AD_CHECK_EQ(parse_duration("abc"), static_cast<int64_t>(-1));
    AD_CHECK_EQ(parse_duration("12:34"), static_cast<int64_t>(-1));   // 缺秒
}

AD_TEST(时间往返一致, "格式化与解析互为逆运算") {
    for (int64_t seconds : {0, 1, 59, 60, 3599, 3600, 3661, 86399}) {
        const int64_t milliseconds = seconds * 1000;
        AD_CHECK_EQ(parse_duration(format_duration(milliseconds)), milliseconds);
    }
}

int main() {
    return adtest::run_all("DLNA SOAP 测试");
}
