#include "task_queue.h"

namespace atp {

bool TaskQueue::push(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_) {
            return false;
        }
        tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
}

bool TaskQueue::pop(std::function<void()>& task) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this]() {
        return closed_ || !tasks_.empty();
    });

    if (tasks_.empty()) {
        // Queue is closed and empty.
        return false;
    }

    task = std::move(tasks_.front());
    tasks_.pop_front();
    return true;
}

PopResult TaskQueue::try_pop(std::function<void()>& task,
                             std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, timeout, [this]() {
        return closed_ || !tasks_.empty();
    });

    if (!tasks_.empty()) {
        task = std::move(tasks_.front());
        tasks_.pop_front();
        return PopResult::kSuccess;
    }

    if (closed_) {
        return PopResult::kClosed;
    }

    return PopResult::kTimeout;
}

void TaskQueue::notify_all() {
    cv_.notify_all();
}

void TaskQueue::close() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
    }
    cv_.notify_all();
}

size_t TaskQueue::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return tasks_.size();
}

bool TaskQueue::is_closed() const {
    std::lock_guard<std::mutex> lock(mu_);
    return closed_;
}

}  // namespace atp
