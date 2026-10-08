#include "RelaySession.h"

#include <adisplay/common/Log.h>

#include "HttpFetch.h"

#include <utility>

namespace adisplay::pipeline {
namespace {

// 拉一份分片的超时。手机上的本地代理就在同一台手机上，正常是毫秒级；
// 十秒还没回来基本就是断了，早点失败让播放器自己重试。
constexpr int kSegmentTimeoutSeconds = 10;

}  // namespace

RelaySession::RelaySession(std::string playlist_url, std::vector<std::string> segment_urls)
    : playlist_url_(std::move(playlist_url)),
      segment_urls_(std::move(segment_urls)),
      slots_(segment_urls_.size()) {}

RelaySession::~RelaySession() {
    stop();
}

bool RelaySession::segment(std::size_t index, std::vector<uint8_t>* out, std::string* error) {
    if (index >= segment_urls_.size()) {
        *error = "分片下标越界";
        return false;
    }
    return wait_for_segment(index, out, error);
}

bool RelaySession::wait_for_segment(std::size_t index, std::vector<uint8_t>* out,
                                    std::string* error) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (stopping_) {
            *error = "中转已停止";
            return false;
        }
        if (broken_) {
            *error = broken_error_;
            return false;
        }

        {
            const Slot& slot = slots_[index];
            if (slot.state == State::Ready) {
                out->assign(slot.data.begin(), slot.data.end());
                return true;
            }
            if (slot.state == State::Failed) {
                // 失败的结果也缓存下来：播放器重试同一份分片时立刻拿到同样的
                // 答复，而不是让每一轮重试都各自卡满一次超时。
                *error = slot.error;
                return false;
            }
        }

        if (!producing_) {
            producing_ = true;
            lock.unlock();
            produce_until(index);
            lock.lock();
            producing_ = false;
            condition_.notify_all();
            continue;   // 回到循环，看看自己那一份好了没有
        }
        condition_.wait(lock);
    }
}

bool RelaySession::init_segment(std::vector<uint8_t>* out, std::string* error) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (stopping_) {
            *error = "中转已停止";
            return false;
        }
        if (broken_) {
            *error = broken_error_;
            return false;
        }
        if (has_init_) {
            out->assign(init_segment_.begin(), init_segment_.end());
            return true;
        }
        if (next_index_ >= slots_.size()) {
            // 一路产到底都没能建起 moov —— 分片全都拿不到。
            *error = last_error_.empty() ? "拿不到任何分片，无法建立初始化段" : last_error_;
            return false;
        }

        if (!producing_) {
            producing_ = true;
            const std::size_t target = next_index_;
            lock.unlock();
            produce_until(target);
            lock.lock();
            producing_ = false;
            condition_.notify_all();
            continue;
        }
        condition_.wait(lock);
    }
}

void RelaySession::produce_until(std::size_t index) {
    for (;;) {
        std::size_t current = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // next_index_ 只在这里推进，而这里只有一个生产者线程能进来。
            if (stopping_ || broken_ || next_index_ > index) {
                return;
            }
            current = next_index_;
        }

        Produced produced = produce_one(current);   // 下载与换封装都在锁外做

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;   // 已经在停了，这一份的产出丢掉
            }
            if (produced.fatal) {
                broken_ = true;
                broken_error_ = produced.error;
                return;
            }
            Slot& slot = slots_[current];
            if (produced.ok) {
                slot.state = State::Ready;
                slot.data = std::move(produced.data);
                if (!has_init_ && remuxer_ != nullptr && remuxer_->has_init_segment()) {
                    init_segment_ = remuxer_->init_segment();
                    has_init_ = true;
                }
            } else {
                slot.state = State::Failed;
                slot.error = produced.error;
                last_error_ = produced.error;
            }
            // 失败也往前走：一份分片拿不到不该把后面的全堵死，播放器能看到一次
            // 跳帧，总好过整段卡住不动。
            next_index_ = current + 1;
        }
    }
}

RelaySession::Produced RelaySession::produce_one(std::size_t index) {
    Produced produced;

    const FetchResult fetched = fetch_url(segment_urls_[index], kSegmentTimeoutSeconds);
    if (!fetched.ok || fetched.body.empty()) {
        produced.error =
            "分片下载失败：" + (fetched.error.empty() ? std::string("响应为空") : fetched.error);
        AD_LOG_WARN("本地中转：分片 {} {}", index, produced.error);
        return produced;
    }

    if (remuxer_ == nullptr) {
        remuxer_ = std::make_unique<Mp4Remuxer>();
    }
    std::string error;
    // 直接拿响应体当输入，不再拷一份 —— 分片是几百 KB 级的。
    const auto* bytes = reinterpret_cast<const uint8_t*>(fetched.body.data());
    if (!remuxer_->append_ts_segment(bytes, fetched.body.size(), &produced.data, &error)) {
        produced.error = error;
        // 复用器内部状态已经不可信（半截的 moof、断掉的时间轴），整个会话作废。
        produced.fatal = true;
        AD_LOG_ERROR("本地中转：分片 {} 换封装失败：{}", index, error);
        return produced;
    }

    produced.ok = true;
    return produced;
}

void RelaySession::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
        return;
    }
    stopping_ = true;
    // 缓存起来的分片加起来大致等于片源大小，停掉就立刻还回去。
    for (Slot& slot : slots_) {
        slot.state = State::Idle;
        slot.data.clear();
        slot.data.shrink_to_fit();
        slot.error.clear();
    }
    init_segment_.clear();
    init_segment_.shrink_to_fit();
    has_init_ = false;
    condition_.notify_all();
}

bool RelaySession::stopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_;
}

}  // namespace adisplay::pipeline
