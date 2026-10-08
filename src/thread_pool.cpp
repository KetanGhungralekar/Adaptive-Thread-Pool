#include "thread_pool.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace atp {

ThreadPool::ThreadPool(size_t initial_workers, size_t min_workers, size_t max_workers)
    : min_workers_(min_workers), max_workers_(max_workers) {
    if (min_workers_ == 0) {
        throw std::invalid_argument("ThreadPool: min_workers must be at least 1");
    }
    if (max_workers_ < min_workers_) {
        throw std::invalid_argument("ThreadPool: max_workers must be >= min_workers");
    }

    size_t target = std::clamp(initial_workers, min_workers_, max_workers_);
    desired_workers_.store(target, std::memory_order_relaxed);
    window_start_time_ = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(workers_mu_);
    for (size_t i = 0; i < target; ++i) {
        spawn_worker_locked();
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    bool expected = false;
    if (!shutdown_.compare_exchange_strong(expected, true)) {
        return;  // Already shut down
    }

    // Close the queue so all workers wake up and drain remaining tasks
    queue_.close();

    std::vector<std::unique_ptr<WorkerHandle>> to_join;
    {
        std::lock_guard<std::mutex> lock(workers_mu_);
        to_join = std::move(workers_);
    }

    for (auto& handle : to_join) {
        if (handle && handle->thread.joinable()) {
            if (handle->thread.get_id() != std::this_thread::get_id()) {
                handle->thread.join();
            }
        }
    }
}

bool ThreadPool::submit_task(std::function<void()> task_fn) {
    if (shutdown_.load(std::memory_order_relaxed)) {
        return false;
    }

    auto enqueue_time = std::chrono::steady_clock::now();
    tasks_in_flight_.fetch_add(1, std::memory_order_relaxed);

    bool pushed = queue_.push([this, fn = std::move(task_fn), enqueue_time]() {
        auto start_time = std::chrono::steady_clock::now();
        try {
            fn();
        } catch (...) {
            // Task exceptions shouldn't break metrics or worker lifecycle
        }
        auto end_time = std::chrono::steady_clock::now();
        record_task_metrics(enqueue_time, start_time, end_time);
    });

    if (!pushed) {
        tasks_in_flight_.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void ThreadPool::record_task_metrics(std::chrono::steady_clock::time_point enqueue_time,
                                     std::chrono::steady_clock::time_point start_time,
                                     std::chrono::steady_clock::time_point end_time) {
    auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - enqueue_time).count();
    auto wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(start_time - enqueue_time).count();

    total_tasks_completed_.fetch_add(1, std::memory_order_relaxed);
    window_tasks_completed_.fetch_add(1, std::memory_order_relaxed);
    window_total_latency_ns_.fetch_add(latency_ns > 0 ? latency_ns : 0, std::memory_order_relaxed);
    window_total_queue_wait_ns_.fetch_add(wait_ns > 0 ? wait_ns : 0, std::memory_order_relaxed);

    if (tasks_in_flight_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(empty_mu_);
        empty_cv_.notify_all();
    }
}

void ThreadPool::wait_idle() {
    std::unique_lock<std::mutex> lock(empty_mu_);
    empty_cv_.wait(lock, [this]() {
        return tasks_in_flight_.load(std::memory_order_relaxed) == 0;
    });
}

void ThreadPool::spawn_worker_locked() {
    auto handle = std::make_unique<WorkerHandle>();
    handle->id = ++next_worker_id_;
    WorkerHandle* raw = handle.get();
    active_workers_.fetch_add(1, std::memory_order_relaxed);
    handle->thread = std::thread([this, raw]() {
        worker_loop(raw);
    });
    workers_.push_back(std::move(handle));
}

void ThreadPool::reap_retired_workers_locked() {
    auto it = workers_.begin();
    while (it != workers_.end()) {
        if ((*it)->finished.load(std::memory_order_acquire)) {
            if ((*it)->thread.joinable() && (*it)->thread.get_id() != std::this_thread::get_id()) {
                (*it)->thread.join();
            }
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
}

void ThreadPool::reap_retired_workers() {
    std::lock_guard<std::mutex> lock(workers_mu_);
    reap_retired_workers_locked();
}

void ThreadPool::worker_loop(WorkerHandle* handle) {
    while (true) {
        // If pool is not shutting down, check if this worker should retire
        if (!shutdown_.load(std::memory_order_relaxed)) {
            size_t active = active_workers_.load(std::memory_order_relaxed);
            size_t desired = desired_workers_.load(std::memory_order_relaxed);
            if (active > desired) {
                if (active_workers_.compare_exchange_strong(active, active - 1)) {
                    // This worker has self-retired
                    handle->finished.store(true, std::memory_order_release);
                    return;
                }
            }
        }

        std::function<void()> task;
        PopResult res = queue_.try_pop(task, std::chrono::milliseconds(50));
        if (res == PopResult::kSuccess) {
            try {
                task();
            } catch (...) {
                // Ignore unhandled task exceptions
            }
        } else if (res == PopResult::kClosed) {
            // Queue is closed AND drained
            break;
        } else if (res == PopResult::kTimeout) {
            // Timed out waiting: loop back to re-check retirement / shutdown
            continue;
        }
    }

    active_workers_.fetch_sub(1, std::memory_order_relaxed);
    handle->finished.store(true, std::memory_order_release);
}

size_t ThreadPool::resize(size_t target_workers) {
    if (shutdown_.load(std::memory_order_relaxed)) {
        return 0;
    }

    size_t clamped = std::clamp(target_workers, min_workers_, max_workers_);

    std::lock_guard<std::mutex> lock(workers_mu_);
    reap_retired_workers_locked();

    desired_workers_.store(clamped, std::memory_order_release);
    size_t current_active = active_workers_.load(std::memory_order_acquire);

    if (clamped > current_active) {
        size_t to_spawn = clamped - current_active;
        for (size_t i = 0; i < to_spawn; ++i) {
            spawn_worker_locked();
        }
    } else if (clamped < current_active) {
        // Wake up sleeping workers so they immediately notice retirement condition
        queue_.notify_all();
    }

    return clamped;
}

WindowMetrics ThreadPool::sample_window_metrics() {
    std::lock_guard<std::mutex> lock(window_mu_);
    auto now = std::chrono::steady_clock::now();
    double duration = std::chrono::duration<double>(now - window_start_time_).count();
    window_start_time_ = now;
    if (duration <= 0.0) duration = 1e-6;

    uint64_t completed = window_tasks_completed_.exchange(0, std::memory_order_relaxed);
    uint64_t lat_ns = window_total_latency_ns_.exchange(0, std::memory_order_relaxed);
    uint64_t wait_ns = window_total_queue_wait_ns_.exchange(0, std::memory_order_relaxed);

    WindowMetrics m;
    m.tasks_completed = completed;
    m.window_duration_sec = duration;
    m.throughput = completed / duration;
    m.avg_latency_ms = completed > 0 ? (static_cast<double>(lat_ns) / completed) / 1e6 : 0.0;
    m.avg_queue_wait_ms = completed > 0 ? (static_cast<double>(wait_ns) / completed) / 1e6 : 0.0;
    m.current_queue_depth = queue_.size();
    m.active_workers = active_workers_.load(std::memory_order_relaxed);

    reap_retired_workers();

    return m;
}

void ThreadPool::reset_window_metrics() {
    std::lock_guard<std::mutex> lock(window_mu_);
    window_start_time_ = std::chrono::steady_clock::now();
    window_tasks_completed_.store(0, std::memory_order_relaxed);
    window_total_latency_ns_.store(0, std::memory_order_relaxed);
    window_total_queue_wait_ns_.store(0, std::memory_order_relaxed);
}

}  // namespace atp
