// ADisplay —— 日志（文档 5 节选型：spdlog）
//
// 设计要点：
//   * 级别取值与 adisplay.h 的 AdLogLevel 一一对应，方便直接跨 C 边界传递。
//   * 日志走 spdlog（写文件 + 控制台），另外可挂一个 sink 把日志转发给界面层，
//     界面上的「日志导出」和实时日志窗口用它（文档 7 第 4 条）。
//   * 文档 6.2 要求：日志里不写入任何画面或敏感数据。调用方自行保证。
#pragma once

#include <spdlog/fmt/fmt.h>  // 跟随 spdlog 选用 external 还是 bundled fmt

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace adisplay::common {

// 与 adisplay.h 的 AdLogLevel 数值保持一致，勿改。
enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Off   = 5,
};

// 转发给界面层的回调。在写日志的线程上同步调用，实现要够快。
// 界面层负责 marshal 到 UI 线程。
using LogSink = std::function<void(LogLevel, std::string_view)>;

// 供日志宏使用的格式化入口。fmt 的编译期格式串检查在这里生效。
template <typename... Args>
std::string format(fmt::format_string<Args...> fmt_string, Args&&... args) {
    return fmt::format(fmt_string, std::forward<Args>(args)...);
}

class Log {
public:
    // 初始化。file_path 为空则只输出到控制台。
    // 可重复调用，会替换掉之前的配置。
    static void init(LogLevel level, const std::string& file_path);

    // 关闭并释放 spdlog 资源。进程退出前调用；之后再用需要重新 init。
    static void shutdown();

    static void set_level(LogLevel level);
    static LogLevel level();

    // 挂接界面层 sink。传空函数对象表示摘掉。
    static void set_sink(LogSink sink);

    // 判断某级别是否会被输出，给宏做短路用，避免白拼字符串。
    static bool enabled(LogLevel level);

    // 写一条。一般走下面的宏，不要直接调。
    static void write(LogLevel level, std::string_view message);

    // 取级别名，用于格式化输出。
    static const char* level_name(LogLevel level);
};

}  // namespace adisplay::common

// ---------------------------------------------------------------------------
// 便捷宏。先判断级别再格式化，避免无谓的字符串拼接开销。
// ---------------------------------------------------------------------------
#define AD_LOG_AT(level_enum, ...)                                                        \
    do {                                                                                  \
        if (::adisplay::common::Log::enabled(level_enum)) {                               \
            ::adisplay::common::Log::write(level_enum,                                    \
                                           ::adisplay::common::format(__VA_ARGS__));      \
        }                                                                                 \
    } while (false)

#define AD_LOG_TRACE(...) AD_LOG_AT(::adisplay::common::LogLevel::Trace, __VA_ARGS__)
#define AD_LOG_DEBUG(...) AD_LOG_AT(::adisplay::common::LogLevel::Debug, __VA_ARGS__)
#define AD_LOG_INFO(...)  AD_LOG_AT(::adisplay::common::LogLevel::Info,  __VA_ARGS__)
#define AD_LOG_WARN(...)  AD_LOG_AT(::adisplay::common::LogLevel::Warn,  __VA_ARGS__)
#define AD_LOG_ERROR(...) AD_LOG_AT(::adisplay::common::LogLevel::Error, __VA_ARGS__)
