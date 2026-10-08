#include <adisplay/common/ThreadPool.h>

namespace adisplay::common {

ThreadPool::ThreadPool(std::size_t thread_count) {
    if (thread_count == 0) {
        thread_count = static_cast<std::size_t>(std::thread::hardware_concurrency());
        if (thread_count == 0) {
            thread_count = 1;
        }
        // 短任务池不需要吃满所有核心，留出余量给解码和渲染线程。
        if (thread_count > 2) {
            thread_count -= 1;
        }
    }

    workers_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        workers_.emplace_back([this]() { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() {
                return stopping_.load(std::memory_order_relaxed) || !tasks_.empty();
            });

            if (tasks_.empty()) {
                // 只有停止时才可能走到这里（被唤醒但队列已空）。
                if (stopping_.load(std::memory_order_relaxed)) {
                    return;
                }
                continue;
            }

            if (stopping_.load(std::memory_order_relaxed) && !drain_on_stop_) {
                // 立刻停止模式：把剩下的任务丢掉。
                std::queue<std::function<void()>> empty;
                tasks_.swap(empty);
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // 任务内部的异常不能让工作线程死掉，否则池子会逐渐失去线程。
        try {
            task();
        } catch (...) {
            // 异常已经通过 packaged_task 送到 future 了，这里只需要吞掉。
        }
    }
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.exchange(true)) {
            // 已经停过了，只等线程收尾。
        }
        drain_on_stop_ = true;
    }
    condition_.notify_all();

    for (std::thread& worker : workers_) {
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
            worker.join();
        }
    }
    workers_.clear();
}

void ThreadPool::shutdown_now() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_.store(true, std::memory_order_relaxed);
        drain_on_stop_ = false;
    }
    condition_.notify_all();

    for (std::thread& worker : workers_) {
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
            worker.join();
        }
    }
    workers_.clear();
}

std::size_t ThreadPool::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

}  // namespace adisplay::common
