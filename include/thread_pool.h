#pragma once

#include "metrics.h"
#include "task_queue.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace atp {

/// A thread pool with fixed or dynamic worker counts.
///
/// Supports generic task submission with std::future, graceful shutdown,
/// and dynamic resizing with safe worker creation and retirement.
class ThreadPool {
public:
    /// Construct a thread pool.
    /// @param initial_workers Number of workers to start with.
    /// @param min_workers Lower bound for dynamic resizing.
    /// @param max_workers Upper bound for dynamic resizing.
    explicit ThreadPool(size_t initial_workers = std::thread::hardware_concurrency(),
                        size_t min_workers = 1,
                        size_t max_workers = 32);

    ~ThreadPool();

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// Submit a callable and any arguments, returning a std::future for the result.
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) 
        -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;

        if (shutdown_.load(std::memory_order_relaxed)) {
            throw std::runtime_error("ThreadPool::submit: cannot submit to shutdown pool");
        }

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<return_type> res = task->get_future();

        auto enqueue_time = std::chrono::steady_clock::now();
        tasks_in_flight_.fetch_add(1, std::memory_order_relaxed);

        bool pushed = queue_.push([this, task, enqueue_time]() {
            auto start_time = std::chrono::steady_clock::now();
            (*task)();
            auto end_time = std::chrono::steady_clock::now();

            record_task_metrics(enqueue_time, start_time, end_time);
        });

        if (!pushed) {
            tasks_in_flight_.fetch_sub(1, std::memory_order_relaxed);
            throw std::runtime_error("ThreadPool::submit: queue closed, cannot accept tasks");
        }

        return res;
    }

    /// Submit a void() task without packaging into a future (minimal overhead).
    bool submit_task(std::function<void()> task_fn);

    /// Resize the thread pool to the desired worker count.
    /// Target count is automatically clamped to [min_workers_, max_workers_].
    /// Returns the new desired worker count.
    size_t resize(size_t target_workers);

    /// Initiate cooperative shutdown.
    /// Disallows new task submissions, drains pending tasks in the queue,
    /// and joins all worker threads.
    void shutdown();

    /// Block calling thread until all currently submitted tasks finish.
    void wait_idle();

    /// Reaps any retired threads that have completed execution.
    void reap_retired_workers();

    /// Sample window metrics since the last call and reset window counters.
    WindowMetrics sample_window_metrics();

    /// Reset window metrics tracking without returning results.
    void reset_window_metrics();

    // Inspectors
    size_t worker_count() const { return active_workers_.load(std::memory_order_relaxed); }
    size_t desired_worker_count() const { return desired_workers_.load(std::memory_order_relaxed); }
    size_t min_workers() const { return min_workers_; }
    size_t max_workers() const { return max_workers_; }
    size_t queue_size() const { return queue_.size(); }
    size_t tasks_in_flight() const { return tasks_in_flight_.load(std::memory_order_relaxed); }
    size_t total_tasks_completed() const { return total_tasks_completed_.load(std::memory_order_relaxed); }
    bool is_shutdown() const { return shutdown_.load(std::memory_order_relaxed); }

private:
    struct WorkerHandle {
        size_t id;
        std::thread thread;
        std::atomic<bool> finished{false};
    };

    void worker_loop(WorkerHandle* handle);
    void spawn_worker_locked();
    void reap_retired_workers_locked();
    void record_task_metrics(std::chrono::steady_clock::time_point enqueue_time,
                             std::chrono::steady_clock::time_point start_time,
                             std::chrono::steady_clock::time_point end_time);

    TaskQueue queue_;
    const size_t min_workers_;
    const size_t max_workers_;

    std::atomic<size_t> active_workers_{0};
    std::atomic<size_t> desired_workers_{0};
    std::atomic<bool> shutdown_{false};

    mutable std::mutex workers_mu_;
    std::vector<std::unique_ptr<WorkerHandle>> workers_;
    size_t next_worker_id_{0};

    // Idle notification
    std::atomic<size_t> tasks_in_flight_{0};
    std::mutex empty_mu_;
    std::condition_variable empty_cv_;

    // Performance tracking atomics
    std::atomic<uint64_t> total_tasks_completed_{0};
    std::atomic<uint64_t> window_tasks_completed_{0};
    std::atomic<uint64_t> window_total_latency_ns_{0};
    std::atomic<uint64_t> window_total_queue_wait_ns_{0};
    std::chrono::steady_clock::time_point window_start_time_;
    std::mutex window_mu_;
};

}  // namespace atp
