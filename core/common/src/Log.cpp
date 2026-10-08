#include <adisplay/common/Log.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace adisplay::common {
namespace {

constexpr const char* kLoggerName = "adisplay";
constexpr std::size_t kMaxFileBytes = 8u * 1024u * 1024u;  // 单个日志文件 8MB
constexpr std::size_t kMaxFileCount = 3;                   // 轮转保留 3 个

std::mutex g_mutex;
std::shared_ptr<spdlog::logger> g_logger;
LogSink g_sink;
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};

// uint32 转 log_clock 的时间点，直接给 fmt 用。
spdlog::level::level_enum to_spdlog(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return spdlog::level::trace;
        case LogLevel::Debug: return spdlog::level::debug;
        case LogLevel::Info:  return spdlog::level::info;
        case LogLevel::Warn:  return spdlog::level::warn;
        case LogLevel::Error: return spdlog::level::err;
        case LogLevel::Off:   return spdlog::level::off;
    }
    return spdlog::level::info;
}

}  // namespace

void Log::init(LogLevel level, const std::string& file_path) {
    std::lock_guard<std::mutex> lock(g_mutex);

    std::vector<spdlog::sink_ptr> sinks;
    // 控制台 sink：Windows 下若没有控制台（GUI 程序）会抛异常，所以吞掉错误。
    try {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
        sinks.push_back(console);
    } catch (const spdlog::spdlog_ex&) {
        // 无控制台，忽略。
    }

    if (!file_path.empty()) {
        try {
            auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                file_path, kMaxFileBytes, kMaxFileCount);
            file->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
            sinks.push_back(file);
        } catch (const spdlog::spdlog_ex&) {
            // 日志文件打不开不应该拖垮整个程序，降级为仅控制台。
        }
    }

    if (sinks.empty()) {
        // 连控制台都没有（例如 Windows GUI 且文件路径无效）：
        // 退化成只把日志送给界面层 sink，仍然可用。
        spdlog::drop(kLoggerName);
        g_logger.reset();
        g_level.store(static_cast<int>(level));
        return;
    }

    auto logger = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());
    logger->set_level(to_spdlog(level));

    // 每条日志都立即落盘。
    //
    // 原来设的是 flush_on(warn) —— 想着「正常日志攒着写，省点 IO」。
    // 实践中的后果是：用户报「搜不到设备」时，日志文件里只有到 WARN 为止的
    // 内容，最关键的几条 INFO（DLNA 就绪、SSDP 在哪些网卡上广播、mDNS 注册
    // 结果）全都还在缓冲区里。那些恰恰是排查时唯一有用的信息。
    //
    // 这个程序的日志量很小（几秒一条），每条 flush 的开销可以忽略。
    // 排查时能看到完整日志，比省这点 IO 重要得多。
    logger->flush_on(spdlog::level::info);
    spdlog::register_or_replace(logger);
    spdlog::set_default_logger(logger);

    g_logger = logger;
    g_level.store(static_cast<int>(level));
}

void Log::shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = nullptr;
    g_logger.reset();
    spdlog::drop(kLoggerName);
    spdlog::shutdown();
}

void Log::set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level.store(static_cast<int>(level));
    if (g_logger) {
        g_logger->set_level(to_spdlog(level));
    }
}

LogLevel Log::level() {
    return static_cast<LogLevel>(g_level.load());
}

void Log::set_sink(LogSink sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = std::move(sink);
}

bool Log::enabled(LogLevel level) {
    const int current = g_level.load(std::memory_order_relaxed);
    return static_cast<int>(level) >= current && current != static_cast<int>(LogLevel::Off);
}

void Log::write(LogLevel level, std::string_view message) {
    // 先把 sink 拷出来，避免持锁调用用户代码造成死锁。
    LogSink sink_copy;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        sink_copy = g_sink;
    }

    if (!message.empty()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_logger) {
            g_logger->log(to_spdlog(level), "{}", message);
        }
    }

    if (sink_copy) {
        sink_copy(level, message);
    }
}

const char* Log::level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off:   return "OFF";
    }
    return "?";
}

}  // namespace adisplay::common
