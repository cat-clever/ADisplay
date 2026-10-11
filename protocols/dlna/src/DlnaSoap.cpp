#include "DlnaSoap.h"

#include <tinyxml2.h>

#include <cstdio>   // sscanf
#include <sstream>

namespace adisplay::dlna::soap {
namespace {

// 去掉首尾空白，包括引号（SOAPAction 头的值可能被引号包着）。
std::string strip(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"';
    };
    while (begin < end && is_space(text[begin])) {
        ++begin;
    }
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// 从元素名里剥掉命名空间前缀（"u:Play" → "Play"）。
std::string strip_namespace(const char* name) {
    if (name == nullptr) {
        return std::string();
    }
    const std::string text(name);
    const std::size_t colon = text.find(':');
    return colon == std::string::npos ? text : text.substr(colon + 1);
}

// 递归收集信封体里所有非空的子元素文本。
// SOAP 参数都是扁平的一层，这个简化处理够了。
void collect_elements(const tinyxml2::XMLElement* element,
                      std::vector<std::pair<std::string, std::string>>& out) {
    for (const tinyxml2::XMLElement* child = element->FirstChildElement();
         child != nullptr; child = child->NextSiblingElement()) {
        const std::string name = strip_namespace(child->Name());
        const char* text = child->GetText();
        if (text != nullptr) {
            out.emplace_back(name, std::string(text));
        } else {
            out.emplace_back(name, std::string());
        }
    }
}

}  // namespace

std::string ActionRequest::get(const std::string& name, const std::string& fallback) const {
    for (const auto& entry : arguments) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    return fallback;
}

std::string extract_service_type(const std::string& soap_action_header) {
    const std::string cleaned = strip(soap_action_header);
    const std::size_t hash = cleaned.find('#');
    if (hash == std::string::npos) {
        return cleaned;
    }
    return cleaned.substr(0, hash);
}

bool parse_action_request(const std::string& soap_action_header,
                          const std::string& body,
                          ActionRequest* out_request,
                          std::string* out_error) {
    if (out_request == nullptr) {
        return false;
    }
    *out_request = ActionRequest{};

    tinyxml2::XMLDocument document;
    if (document.Parse(body.c_str(), body.size()) != tinyxml2::XML_SUCCESS) {
        if (out_error != nullptr) {
            *out_error = "SOAP 请求体不是合法 XML";
        }
        return false;
    }

    const tinyxml2::XMLElement* envelope = document.RootElement();
    if (envelope == nullptr || strip_namespace(envelope->Name()) != "Envelope") {
        if (out_error != nullptr) {
            *out_error = "SOAP 请求缺少 Envelope";
        }
        return false;
    }

    // 找 Body，再找 Body 下的第一个元素，那就是动作本身。
    const tinyxml2::XMLElement* body_element = nullptr;
    for (const tinyxml2::XMLElement* child = envelope->FirstChildElement();
         child != nullptr; child = child->NextSiblingElement()) {
        if (strip_namespace(child->Name()) == "Body") {
            body_element = child;
            break;
        }
    }
    if (body_element == nullptr) {
        if (out_error != nullptr) {
            *out_error = "SOAP 请求缺少 Body";
        }
        return false;
    }

    const tinyxml2::XMLElement* action = body_element->FirstChildElement();
    if (action == nullptr) {
        if (out_error != nullptr) {
            *out_error = "SOAP Body 是空的";
        }
        return false;
    }

    // 动作名以信封体为准：部分实现在 SOAPAction 头里写得不准或干脆不写。
    out_request->action_name = strip_namespace(action->Name());
    out_request->service_type = extract_service_type(soap_action_header);

    if (out_request->action_name.empty()) {
        if (out_error != nullptr) {
            *out_error = "无法识别 SOAP 动作名";
        }
        return false;
    }

    collect_elements(action, out_request->arguments);
    return true;
}

std::string escape_xml(const std::string& text) {
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (const char c : text) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

std::string build_action_response(const std::string& service_type,
                                  const std::string& action_name,
                                  const std::vector<std::pair<std::string, std::string>>& results) {
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        // SOAP 信封的命名空间必须一字不差，手机端会据此解析。
        << "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        << "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\n"
        << "<s:Body>\n"
        << "<u:" << action_name << "Response xmlns:u=\"" << service_type << "\">\n";

    for (const auto& entry : results) {
        out << "<" << entry.first << ">"
            << escape_xml(entry.second)
            << "</" << entry.first << ">\n";
    }

    out << "</u:" << action_name << "Response>\n"
        << "</s:Body>\n"
        << "</s:Envelope>\n";
    return out.str();
}

std::string build_fault(int error_code, const std::string& description) {
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        << "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        << "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\n"
        << "<s:Body>\n"
        << "<s:Fault>\n"
        << "<faultcode>s:Client</faultcode>\n"
        << "<faultstring>UPnPError</faultstring>\n"
        << "<detail>\n"
        << "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">\n"
        << "<errorCode>" << error_code << "</errorCode>\n"
        << "<errorDescription>" << escape_xml(description) << "</errorDescription>\n"
        << "</UPnPError>\n"
        << "</detail>\n"
        << "</s:Fault>\n"
        << "</s:Body>\n"
        << "</s:Envelope>\n";
    return out.str();
}

std::string format_duration(int64_t milliseconds) {
    if (milliseconds < 0) {
        return "0:00:00";
    }
    const int64_t total_seconds = milliseconds / 1000;
    const int64_t hours = total_seconds / 3600;
    const int64_t minutes = (total_seconds % 3600) / 60;
    const int64_t seconds = total_seconds % 60;

    std::ostringstream out;
    // UPnP 用 1 位小时 + 2 位分秒，与 ISO 8601 的 2 位小时不同。
    out << hours << ':'
        << (minutes < 10 ? "0" : "") << minutes << ':'
        << (seconds < 10 ? "0" : "") << seconds;
    return out.str();
}

int64_t parse_duration(const std::string& text) {
    // 接受 "H:MM:SS" 与 "H:MM:SS.mmm" 两种写法。
    // 用 long long 接 sscanf 的结果：int64_t 在 LP64 平台上是 long、
    // 在 Windows 上是 long long，直接 reinterpret_cast 指针是类型双关。
    long long hours = 0;
    long long minutes = 0;
    long long seconds = 0;
    long long milliseconds = 0;

    const int matched = std::sscanf(text.c_str(), "%lld:%lld:%lld.%lld",
                                    &hours, &minutes, &seconds, &milliseconds);
    if (matched < 3) {
        return -1;
    }
    if (hours < 0 || minutes < 0 || seconds < 0 || milliseconds < 0) {
        return -1;
    }
    return ((hours * 3600) + (minutes * 60) + seconds) * 1000 + milliseconds;
}

}  // namespace adisplay::dlna::soap
