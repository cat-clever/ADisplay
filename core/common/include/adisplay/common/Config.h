// ADisplay —— 配置持久化
//
// 文档 2.4 要求「名称保存在本地配置文件中，软件升级后保留」。
// 这里是一个扁平的键值存储，键用点分命名（"device.name"、"network.airplay_port"），
// 后端是 JSON 文件，便于用户自己编辑和排障。
//
// 写盘采用「先写临时文件再原子改名」，避免掉电或崩溃把配置写坏 ——
// 配置坏了会连带 deviceid 丢失，已配对的手机就得重新配对。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace adisplay::common {

class Config {
public:
    Config() = default;

    // 读配置。文件不存在或解析失败时返回一份空配置，并填充 out_error。
    static Config load(const std::string& path, std::string* out_error = nullptr);

    // 原子写盘。父目录会自动创建。
    bool save(const std::string& path, std::string* out_error = nullptr) const;

    // --- 读 ---
    std::string get_string(const std::string& key, const std::string& fallback) const;
    int64_t     get_int(const std::string& key, int64_t fallback) const;
    bool        get_bool(const std::string& key, bool fallback) const;
    std::vector<std::string> get_string_list(const std::string& key) const;

    bool has(const std::string& key) const;

    // --- 写 ---
    void set_string(const std::string& key, const std::string& value);
    void set_int(const std::string& key, int64_t value);
    void set_bool(const std::string& key, bool value);
    void set_string_list(const std::string& key, const std::vector<std::string>& values);

    void remove(const std::string& key);
    void clear();

    // 所有键，升序。调试用。
    std::vector<std::string> keys() const;

    // --- 平台默认路径 ---
    // macOS:   ~/Library/Application Support/ADisplay/config.json
    // Windows: %APPDATA%\ADisplay\config.json
    // Linux:   $XDG_CONFIG_HOME/adisplay/config.json
    static std::string default_config_path();

    // 同上目录下的 adisplay.log。
    static std::string default_log_path();

private:
    std::map<std::string, std::string> values_;  // 统一按字符串存，读出时再转
};

}  // namespace adisplay::common
