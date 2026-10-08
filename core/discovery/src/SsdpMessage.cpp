#include "SsdpMessage.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <sstream>

namespace adisplay::discovery::ssdp {

std::string to_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trim(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string header_value(const std::string& message, const std::string& name) {
    const std::string lower_name = to_lower(name);
    std::istringstream stream(message);
    std::string line;
    bool first_line = true;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (first_line) {
            first_line = false;
            continue;   // 跳过请求行 / 状态行
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (to_lower(line.substr(0, colon)) == lower_name) {
            return trim(line.substr(colon + 1));
        }
    }
    return std::string();
}

std::string http_date() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    ::gmtime_s(&utc, &now);
#else
    ::gmtime_r(&now, &utc);
#endif
    char buffer[64] = {0};
    std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &utc);
    return std::string(buffer);
}

std::vector<NotificationTarget> build_targets(const SsdpAdvertisement& advertisement) {
    std::vector<NotificationTarget> targets;

    const auto append = [&targets, &advertisement](const std::string& nt) {
        targets.push_back(NotificationTarget{nt, advertisement.udn + "::" + nt});
    };

    append("upnp:rootdevice");
    append(advertisement.udn);
    append(advertisement.device_type);
    for (const std::string& service : advertisement.service_types) {
        append(service);
    }
    return targets;
}

std::string build_alive_message(const SsdpAdvertisement& advertisement,
                                const NotificationTarget& target) {
    std::ostringstream out;
    out << "NOTIFY * HTTP/1.1\r\n"
        << "HOST: " << kMulticastAddress << ":" << kPort << "\r\n"
        << "CACHE-CONTROL: max-age=" << advertisement.max_age_seconds << "\r\n"
        << "LOCATION: " << advertisement.location << "\r\n"
        << "NT: " << target.nt << "\r\n"
        << "NTS: ssdp:alive\r\n"
        << "SERVER: " << advertisement.server_header << "\r\n"
        << "USN: " << target.usn << "\r\n"
        << "\r\n";
    return out.str();
}

std::string build_byebye_message(const SsdpAdvertisement& advertisement,
                                 const NotificationTarget& target) {
    (void)advertisement;   // byebye 不带 LOCATION 与 CACHE-CONTROL
    std::ostringstream out;
    out << "NOTIFY * HTTP/1.1\r\n"
        << "HOST: " << kMulticastAddress << ":" << kPort << "\r\n"
        << "NT: " << target.nt << "\r\n"
        << "NTS: ssdp:byebye\r\n"
        << "USN: " << target.usn << "\r\n"
        << "\r\n";
    return out.str();
}

std::string build_search_response(const SsdpAdvertisement& advertisement,
                                  const std::string& search_target) {
    std::ostringstream out;
    out << "HTTP/1.1 200 OK\r\n"
        << "CACHE-CONTROL: max-age=" << advertisement.max_age_seconds << "\r\n"
        << "DATE: " << http_date() << "\r\n"
        // EXT 头是 UPnP 规范要求的，值必须为空。
        << "EXT:\r\n"
        << "LOCATION: " << advertisement.location << "\r\n"
        << "SERVER: " << advertisement.server_header << "\r\n"
        << "ST: " << search_target << "\r\n"
        << "USN: " << advertisement.udn << "::" << search_target << "\r\n"
        << "\r\n";
    return out.str();
}

bool matches_search_target(const SsdpAdvertisement& advertisement,
                           const std::string& search_target) {
    if (search_target.empty()) {
        return false;
    }
    if (search_target == "ssdp:all") {
        return true;
    }
    if (search_target == "upnp:rootdevice" ||
        search_target == advertisement.udn ||
        search_target == advertisement.device_type) {
        return true;
    }
    for (const std::string& service : advertisement.service_types) {
        if (search_target == service) {
            return true;
        }
    }
    return false;
}

bool is_search_request(const std::string& message) {
    return message.compare(0, 8, "M-SEARCH") == 0;
}

}  // namespace adisplay::discovery::ssdp
