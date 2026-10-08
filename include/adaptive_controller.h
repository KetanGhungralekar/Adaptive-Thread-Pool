#pragma once

#include "metrics.h"
#include "thread_pool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace atp {

struct ControllerConfig {
    std::chrono::milliseconds window_interval{100};
    double epsilon = 0.02;        // 2% minimum improvement threshold
    int cooldown_windows = 2;     // Windows to wait before reversing again
    double alpha = 0.35;          // EMA smoothing factor for throughput
    size_t min_workers = 1;
    size_t max_workers = 32;
    int max_step = 4;             // Momentum: maximum step size acceleration
};

/// Hill-climbing adaptive controller (Strategy B: Directional with Momentum).
///
/// Runs periodically in a background thread, samples window throughput metrics,
/// computes an EMA-smoothed score, and adjusts the pool's worker count.
/// Includes an epsilon threshold to avoid jitter on flat surfaces, a reversal
/// cooldown to prevent oscillation, and automatic boundary clamping.
class AdaptiveController {
public:
    AdaptiveController(ThreadPool& pool, ControllerConfig config = {});
    ~AdaptiveController();

    // Non-copyable, non-movable
    AdaptiveController(const AdaptiveController&) = delete;
    AdaptiveController& operator=(const AdaptiveController&) = delete;

    /// Start the background auto-tuning loop.
    void start();

    /// Stop the background auto-tuning loop.
    void stop();

    /// Execute a single tuning step synchronously (useful for deterministic tests).
    void step();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    /// Returns time-series of (elapsed_seconds, worker_count) recorded across decisions.
    std::vector<std::pair<double, size_t>> get_worker_history() const;

    /// Number of worker count adjustments made.
    size_t resize_count() const { return resize_count_.load(std::memory_order_relaxed); }

    int current_direction() const { return direction_; }
    double current_smoothed_score() const { return smoothed_score_; }

private:
    void loop();
    void record_history(size_t workers);

    ThreadPool& pool_;
    ControllerConfig config_;

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex cv_mu_;
    std::condition_variable cv_;

    // Controller algorithm state
    int direction_{1};            // +1 (scaling up) or -1 (scaling down)
    int step_size_{1};            // Momentum accelerated step size
    double prev_score_{0.0};
    double smoothed_score_{0.0};
    int cooldown_counter_{0};
    bool first_sample_{true};
    std::atomic<size_t> resize_count_{0};

    std::chrono::steady_clock::time_point start_time_;
    mutable std::mutex history_mu_;
    std::vector<std::pair<double, size_t>> history_;
};

}  // namespace atp
