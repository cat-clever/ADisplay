#include <adisplay/common/Config.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <pwd.h>
#  include <unistd.h>
#else
#  include <pwd.h>
#  include <unistd.h>
#endif

namespace adisplay::common {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// 取用户主目录。失败返回空串。
std::string home_directory() {
#if defined(_WIN32)
    const char* profile = std::getenv("USERPROFILE");
    if (profile != nullptr && profile[0] != '\0') {
        return std::string(profile);
    }
    return std::string();
#else
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return std::string(home);
    }
    // HOME 没设（某些服务/容器环境）时退回 passwd。
    const struct passwd* pw = ::getpwuid(::getuid());
    if (pw != nullptr && pw->pw_dir != nullptr) {
        return std::string(pw->pw_dir);
    }
    return std::string();
#endif
}

std::string base_directory() {
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata != nullptr && appdata[0] != '\0') {
        return std::string(appdata) + "\\ADisplay";
    }
    return home_directory() + "\\AppData\\Roaming\\ADisplay";
#elif defined(__APPLE__)
    return home_directory() + "/Library/Application Support/ADisplay";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
        return std::string(xdg) + "/adisplay";
    }
    return home_directory() + "/.config/adisplay";
#endif
}

// 递归创建父目录。
bool ensure_parent_directory(const std::string& file_path, std::string* out_error) {
    std::error_code ec;
    const fs::path parent = fs::path(file_path).parent_path();
    if (parent.empty()) {
        return true;
    }
    if (fs::exists(parent, ec)) {
        return true;
    }
    fs::create_directories(parent, ec);
    if (ec) {
        if (out_error != nullptr) {
            *out_error = "无法创建目录 " + parent.string() + "：" + ec.message();
        }
        return false;
    }
    return true;
}

}  // namespace

Config Config::load(const std::string& path, std::string* out_error) {
    Config config;

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        // 首次运行，不是错误。
        return config;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (out_error != nullptr) {
            *out_error = "配置文件打不开：" + path;
        }
        return config;
    }

    json document;
    try {
        input >> document;
    } catch (const json::exception& ex) {
        if (out_error != nullptr) {
            *out_error = std::string("配置文件解析失败：") + ex.what();
        }
        return config;
    }

    if (!document.is_object()) {
        if (out_error != nullptr) {
            *out_error = "配置文件根节点不是 JSON 对象";
        }
        return config;
    }

    // 只接受标量与字符串数组，其余类型（嵌套对象、null）一律忽略，
    // 保证用户手改配置文件写错格式时不会让程序起不来。
    for (auto it = document.begin(); it != document.end(); ++it) {
        const std::string& key = it.key();
        const json& value = it.value();

        if (value.is_string()) {
            config.values_[key] = value.get<std::string>();
        } else if (value.is_boolean()) {
            config.values_[key] = value.get<bool>() ? "true" : "false";
        } else if (value.is_number_integer()) {
            config.values_[key] = std::to_string(value.get<int64_t>());
        } else if (value.is_number_unsigned()) {
            config.values_[key] = std::to_string(value.get<uint64_t>());
        } else if (value.is_number_float()) {
            config.values_[key] = std::to_string(value.get<double>());
        } else if (value.is_array()) {
            // 字符串数组用 '\n' 连接存放，读的时候再拆。
            std::string joined;
            for (const json& element : value) {
                if (!element.is_string()) {
                    continue;
                }
                if (!joined.empty()) {
                    joined.push_back('\n');
                }
                joined += element.get<std::string>();
            }
            config.values_[key] = joined;
        }
    }

    return config;
}

bool Config::save(const std::string& path, std::string* out_error) const {
    if (!ensure_parent_directory(path, out_error)) {
        return false;
    }

    json document = json::object();
    for (const auto& entry : values_) {
        document[entry.first] = entry.second;
    }

    // 先写同目录下的临时文件，再原子改名。这样即使写到一半崩了，
    // 原来的配置文件仍然完好。
    const std::string temp_path = path + ".tmp";
    {
        std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (out_error != nullptr) {
                *out_error = "无法写入临时配置文件：" + temp_path;
            }
            return false;
        }
        output << document.dump(2) << '\n';
        output.flush();
        if (!output) {
            if (out_error != nullptr) {
                *out_error = "写入配置文件时出错：" + temp_path;
            }
            return false;
        }
    }

    std::error_code ec;
    fs::rename(temp_path, path, ec);
    if (ec) {
        // Windows 上目标已存在时 rename 会失败，退回先删再改名。
        fs::remove(path, ec);
        ec.clear();
        fs::rename(temp_path, path, ec);
        if (ec) {
            if (out_error != nullptr) {
                *out_error = "替换配置文件失败：" + ec.message();
            }
            return false;
        }
    }

    return true;
}

std::string Config::get_string(const std::string& key, const std::string& fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return fallback;
    }
    return it->second;
}

int64_t Config::get_int(const std::string& key, int64_t fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return fallback;
    }
    try {
        return std::stoll(it->second);
    } catch (const std::exception&) {
        return fallback;
    }
}

bool Config::get_bool(const std::string& key, bool fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return fallback;
    }
    const std::string& text = it->second;
    if (text == "true" || text == "1" || text == "yes" || text == "on") {
        return true;
    }
    if (text == "false" || text == "0" || text == "no" || text == "off") {
        return false;
    }
    return fallback;
}

std::vector<std::string> Config::get_string_list(const std::string& key) const {
    std::vector<std::string> result;
    const auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) {
        return result;
    }
    std::string current;
    for (const char c : it->second) {
        if (c == '\n') {
            result.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    result.push_back(current);
    return result;
}

bool Config::has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

void Config::set_string(const std::string& key, const std::string& value) {
    values_[key] = value;
}

void Config::set_int(const std::string& key, int64_t value) {
    values_[key] = std::to_string(value);
}

void Config::set_bool(const std::string& key, bool value) {
    values_[key] = value ? "true" : "false";
}

void Config::set_string_list(const std::string& key, const std::vector<std::string>& values) {
    std::string joined;
    for (const std::string& value : values) {
        if (!joined.empty()) {
            joined.push_back('\n');
        }
        joined += value;
    }
    values_[key] = joined;
}

void Config::remove(const std::string& key) {
    values_.erase(key);
}

void Config::clear() {
    values_.clear();
}

std::vector<std::string> Config::keys() const {
    std::vector<std::string> result;
    result.reserve(values_.size());
    for (const auto& entry : values_) {
        result.push_back(entry.first);
    }
    return result;
}

std::string Config::default_config_path() {
    return base_directory() + "/config.json";
}

std::string Config::default_log_path() {
    return base_directory() + "/adisplay.log";
}

}  // namespace adisplay::common
