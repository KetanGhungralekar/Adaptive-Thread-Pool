#pragma once

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace atp {

/// Metrics captured over a single adaptive controller measurement window.
struct WindowMetrics {
    size_t tasks_completed = 0;
    double window_duration_sec = 0.0;
    double throughput = 0.0;          // tasks / second
    double avg_latency_ms = 0.0;       // completion - enqueue
    double avg_queue_wait_ms = 0.0;    // start - enqueue
    size_t current_queue_depth = 0;
    size_t active_workers = 0;
};

/// Summary metrics for an entire benchmark run.
struct BenchmarkResult {
    std::string workload_name;
    std::string pool_mode;              // "fixed" or "adaptive"
    size_t configured_workers = 0;      // fixed worker count, or initial worker count
    size_t min_workers_seen = 0;
    size_t max_workers_seen = 0;
    size_t worker_changes = 0;
    size_t total_tasks = 0;
    double wall_time_sec = 0.0;
    double throughput = 0.0;            // tasks / second
    double avg_latency_ms = 0.0;
    double p50_latency_ms = 0.0;
    double p95_latency_ms = 0.0;
    double p99_latency_ms = 0.0;
    double max_latency_ms = 0.0;
    double avg_queue_wait_ms = 0.0;
    std::vector<std::pair<double, size_t>> worker_history; // (elapsed_sec, worker_count)
};

}  // namespace atp
