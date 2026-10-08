// ADisplay —— 线程池
//
// 文档 4.4 的线程模型里，网络 IO、解密解复用、解码、渲染、音频各有自己的
// 线程。这里提供一个通用的池子给短任务用（日志落盘、会话清理、mDNS 重注册
// 等），长期存在的流水线线程仍由各模块自己持有。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace adisplay::common {

class ThreadPool {
public:
    // thread_count 为 0 时取硬件并发数（至少 1）。
    explicit ThreadPool(std::size_t thread_count = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 提交一个任务，返回它的 future。任务抛出的异常会经 future 传回调用方。
    template <typename F, typename... Args>
    auto submit(F&& function, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type> {
        using ResultType = typename std::invoke_result<F, Args...>::type;

        auto task = std::make_shared<std::packaged_task<ResultType()>>(
            std::bind(std::forward<F>(function), std::forward<Args>(args)...));
        std::future<ResultType> future = task->get_future();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                // 池子已经关了，交回一个空的 future 让调用方自己判断。
                return std::future<ResultType>();
            }
            tasks_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return future;
    }

    // 停止接收新任务，跑完队列里已有的任务后返回。
    void shutdown();

    // 停止并丢弃尚未开始的任务，已在跑的任务不打断。
    void shutdown_now();

    bool is_stopping() const { return stopping_.load(std::memory_order_relaxed); }

    std::size_t thread_count() const { return workers_.size(); }

    // 当前排队等待的任务数，用于诊断。
    std::size_t pending_count() const;

private:
    void worker_loop();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    bool drain_on_stop_ = true;
};

}  // namespace adisplay::common
