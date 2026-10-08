#include "adaptive_controller.h"

#include <algorithm>
#include <iostream>

namespace atp {

AdaptiveController::AdaptiveController(ThreadPool& pool, ControllerConfig config)
    : pool_(pool), config_(config) {
    start_time_ = std::chrono::steady_clock::now();
    record_history(pool_.worker_count());
}

AdaptiveController::~AdaptiveController() {
    stop();
}

void AdaptiveController::start() {
    if (running_.exchange(true)) {
        return;
    }
    start_time_ = std::chrono::steady_clock::now();
    first_sample_ = true;
    prev_score_ = 0.0;
    smoothed_score_ = 0.0;
    cooldown_counter_ = 0;
    direction_ = 1;
    step_size_ = 1;

    pool_.reset_window_metrics();
    thread_ = std::thread([this]() {
        loop();
    });
}

void AdaptiveController::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        if (thread_.get_id() != std::this_thread::get_id()) {
            thread_.join();
        }
    }
}

void AdaptiveController::loop() {
    while (running_.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lock(cv_mu_);
        if (cv_.wait_for(lock, config_.window_interval, [this]() {
            return !running_.load(std::memory_order_relaxed);
        })) {
            break;
        }
        step();
    }
}

void AdaptiveController::step() {
    WindowMetrics m = pool_.sample_window_metrics();

    // If completely idle with no queued tasks, don't make erratic tuning decisions
    if (m.tasks_completed == 0 && m.current_queue_depth == 0) {
        return;
    }

    double raw_score = m.throughput;
    if (first_sample_) {
        smoothed_score_ = raw_score;
        prev_score_ = raw_score;
        first_sample_ = false;
        return;
    }

    smoothed_score_ = config_.alpha * raw_score + (1.0 - config_.alpha) * smoothed_score_;
    double current_score = smoothed_score_;

    if (cooldown_counter_ > 0) {
        --cooldown_counter_;
    }

    size_t current_desired = pool_.desired_worker_count();

    // Strategy B: Directional Hill Climbing with Momentum
    if (current_score > prev_score_ * (1.0 + config_.epsilon)) {
        // Improvement verified! Accelerate momentum in direction_
        prev_score_ = current_score;
        step_size_ = std::min(step_size_ + 1, std::max(1, config_.max_step));
        int delta = direction_ * step_size_;
        int next_workers = static_cast<int>(current_desired) + delta;
        next_workers = std::clamp(next_workers,
                                  static_cast<int>(config_.min_workers),
                                  static_cast<int>(config_.max_workers));

        if (static_cast<size_t>(next_workers) != current_desired) {
            pool_.resize(next_workers);
            resize_count_.fetch_add(1, std::memory_order_relaxed);
            record_history(next_workers);
        } else {
            // Hit clamp boundary (min or max workers)
            if (cooldown_counter_ == 0) {
                direction_ = -direction_;
                step_size_ = 1;
                cooldown_counter_ = config_.cooldown_windows;
            }
        }
    } else {
        // Flat or degraded throughput
        if (cooldown_counter_ == 0) {
            // Reverse direction and take exploratory step with reset momentum
            direction_ = -direction_;
            step_size_ = 1;
            cooldown_counter_ = config_.cooldown_windows;
            prev_score_ = current_score;

            int next_workers = static_cast<int>(current_desired) + direction_;
            next_workers = std::clamp(next_workers,
                                      static_cast<int>(config_.min_workers),
                                      static_cast<int>(config_.max_workers));

            if (static_cast<size_t>(next_workers) != current_desired) {
                pool_.resize(next_workers);
                resize_count_.fetch_add(1, std::memory_order_relaxed);
                record_history(next_workers);
            }
        } else {
            prev_score_ = current_score;
        }
    }
}

void AdaptiveController::record_history(size_t workers) {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start_time_).count();
    std::lock_guard<std::mutex> lock(history_mu_);
    history_.emplace_back(elapsed, workers);
}

std::vector<std::pair<double, size_t>> AdaptiveController::get_worker_history() const {
    std::lock_guard<std::mutex> lock(history_mu_);
    return history_;
}

}  // namespace atp
