#pragma once

#include "metrics.h"
#include "thread_pool.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace atp {

struct TaskTimestamp {
    std::chrono::steady_clock::time_point enqueue_time;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
};

enum class WorkloadType {
    kCpuBound,
    kWaitHeavy,
    kMixed,
    kBursty,
    kPhaseChanging
};

inline std::string to_string(WorkloadType type) {
    switch (type) {
        case WorkloadType::kCpuBound: return "CPU-bound";
        case WorkloadType::kWaitHeavy: return "Wait-heavy";
        case WorkloadType::kMixed: return "Mixed (70/30)";
        case WorkloadType::kBursty: return "Bursty";
        case WorkloadType::kPhaseChanging: return "Phase-changing";
    }
    return "Unknown";
}

namespace detail {

template <typename T>
inline void do_not_optimize(T&& val) {
    asm volatile("" : "+r,m"(val) : : "memory");
}

inline bool is_prime(uint64_t n) {
    if (n <= 1) return false;
    if (n <= 3) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    for (uint64_t i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return false;
    }
    return true;
}

inline uint64_t cpu_kernel(uint64_t seed) {
    uint64_t count = 0;
    uint64_t start_val = 20000000ULL + (seed % 10000ULL) * 50ULL;
    for (uint64_t candidate = start_val; candidate < start_val + 12000ULL; ++candidate) {
        if (is_prime(candidate)) {
            ++count;
        }
    }
    do_not_optimize(count);
    return count;
}

inline void wait_kernel(int duration_ms = 2) {
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
}

}  // namespace detail

/// Executes a workload against @p pool and returns a BenchmarkResult with all latency and throughput metrics.
inline BenchmarkResult execute_workload(WorkloadType type,
                                        ThreadPool& pool,
                                        const std::string& pool_mode = "fixed",
                                        double scale_factor = 1.0) {
    size_t total_tasks = 0;
    switch (type) {
        case WorkloadType::kCpuBound:
            total_tasks = static_cast<size_t>(3000 * scale_factor);
            break;
        case WorkloadType::kWaitHeavy:
            total_tasks = static_cast<size_t>(3000 * scale_factor);
            break;
        case WorkloadType::kMixed:
            total_tasks = static_cast<size_t>(3000 * scale_factor);
            break;
        case WorkloadType::kBursty:
            total_tasks = static_cast<size_t>(3000 * scale_factor);
            break;
        case WorkloadType::kPhaseChanging:
            total_tasks = static_cast<size_t>(9000 * scale_factor);
            break;
    }
    if (total_tasks == 0) total_tasks = 100;

    std::vector<TaskTimestamp> timestamps(total_tasks);
    size_t initial_workers = pool.worker_count();

    auto run_start = std::chrono::steady_clock::now();

    if (type == WorkloadType::kCpuBound) {
        for (size_t i = 0; i < total_tasks; ++i) {
            timestamps[i].enqueue_time = std::chrono::steady_clock::now();
            pool.submit_task([&timestamps, i]() {
                timestamps[i].start_time = std::chrono::steady_clock::now();
                detail::cpu_kernel(i);
                timestamps[i].end_time = std::chrono::steady_clock::now();
            });
        }
    } else if (type == WorkloadType::kWaitHeavy) {
        for (size_t i = 0; i < total_tasks; ++i) {
            timestamps[i].enqueue_time = std::chrono::steady_clock::now();
            pool.submit_task([&timestamps, i]() {
                timestamps[i].start_time = std::chrono::steady_clock::now();
                detail::wait_kernel(2);
                timestamps[i].end_time = std::chrono::steady_clock::now();
            });
        }
    } else if (type == WorkloadType::kMixed) {
        for (size_t i = 0; i < total_tasks; ++i) {
            bool is_cpu = (i % 10 < 7);  // 70% CPU, 30% wait
            timestamps[i].enqueue_time = std::chrono::steady_clock::now();
            pool.submit_task([&timestamps, i, is_cpu]() {
                timestamps[i].start_time = std::chrono::steady_clock::now();
                if (is_cpu) {
                    detail::cpu_kernel(i);
                } else {
                    detail::wait_kernel(2);
                }
                timestamps[i].end_time = std::chrono::steady_clock::now();
            });
        }
    } else if (type == WorkloadType::kBursty) {
        size_t b1 = total_tasks / 5;        // 20%
        size_t b2 = total_tasks * 3 / 5;    // 60%
        size_t b3 = total_tasks - b1 - b2;  // 20%

        size_t idx = 0;
        // Batch 1
        for (size_t i = 0; i < b1; ++i, ++idx) {
            timestamps[idx].enqueue_time = std::chrono::steady_clock::now();
            pool.submit_task([&timestamps, idx]() {
                timestamps[idx].start_time = std::chrono::steady_clock::now();
                detail::cpu_kernel(idx);
                timestamps[idx].end_time = std::chrono::steady_clock::now();
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Batch 2 (large burst)
        for (size_t i = 0; i < b2; ++i, ++idx) {
            timestamps[idx].enqueue_time = std::chrono::steady_clock::now();
            pool.submit_task([&timestamps, idx]() {
                timestamps[idx].start_time = std::chrono::steady_clock::now();
                detail::cpu_kernel(idx);
                timestamps[idx].end_time = std::chrono::steady_clock::now();
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Batch 3
        for (size_t i = 0; i < b3; ++i, ++idx) {
            timestamps[idx].enqueue_time = std::chrono::steady_clock::now();
            pool.submit_task([&timestamps, idx]() {
                timestamps[idx].start_time = std::chrono::steady_clock::now();
                detail::cpu_kernel(idx);
                timestamps[idx].end_time = std::chrono::steady_clock::now();
            });
        }
    } else if (type == WorkloadType::kPhaseChanging) {
        size_t phase_size = total_tasks / 3;
        size_t idx = 0;

        // Phase 1: CPU-bound
        for (size_t i = 0; i < phase_size; ++i, ++idx) {
            timestamps[idx].enqueue_time = std::chrono::steady_clock::now();
            pool.submit_task([&timestamps, idx]() {
                timestamps[idx].start_time = std::chrono::steady_clock::now();
                detail::cpu_kernel(idx);
                timestamps[idx].end_time = std::chrono::steady_clock::now();
            });
        }

        // Phase 2: Wait-heavy
        for (size_t i = 0; i < phase_size; ++i, ++idx) {
            timestamps[idx].enqueue_time = std::chrono::steady_clock::now();
            pool.submit_task([&timestamps, idx]() {
                timestamps[idx].start_time = std::chrono::steady_clock::now();
                detail::wait_kernel(2);
                timestamps[idx].end_time = std::chrono::steady_clock::now();
            });
        }

        // Phase 3: CPU-bound
        for (size_t i = 0; i < total_tasks - phase_size * 2; ++i, ++idx) {
            timestamps[idx].enqueue_time = std::chrono::steady_clock::now();
            pool.submit_task([&timestamps, idx]() {
                timestamps[idx].start_time = std::chrono::steady_clock::now();
                detail::cpu_kernel(idx);
                timestamps[idx].end_time = std::chrono::steady_clock::now();
            });
        }
    }

    pool.wait_idle();
    auto run_end = std::chrono::steady_clock::now();

    // Compute timing and latency metrics
    double wall_time_sec = std::chrono::duration<double>(run_end - run_start).count();
    if (wall_time_sec <= 0.0) wall_time_sec = 1e-6;

    std::vector<double> latencies_ms;
    latencies_ms.reserve(total_tasks);
    std::vector<double> wait_times_ms;
    wait_times_ms.reserve(total_tasks);

    double sum_latency_ms = 0.0;
    double sum_wait_ms = 0.0;

    for (const auto& ts : timestamps) {
        double lat_ms = std::chrono::duration<double, std::milli>(ts.end_time - ts.enqueue_time).count();
        double wait_ms = std::chrono::duration<double, std::milli>(ts.start_time - ts.enqueue_time).count();
        latencies_ms.push_back(lat_ms);
        wait_times_ms.push_back(wait_ms);
        sum_latency_ms += lat_ms;
        sum_wait_ms += wait_ms;
    }

    std::sort(latencies_ms.begin(), latencies_ms.end());

    BenchmarkResult res;
    res.workload_name = to_string(type);
    res.pool_mode = pool_mode;
    res.configured_workers = initial_workers;
    res.total_tasks = total_tasks;
    res.wall_time_sec = wall_time_sec;
    res.throughput = total_tasks / wall_time_sec;
    res.avg_latency_ms = sum_latency_ms / total_tasks;
    res.avg_queue_wait_ms = sum_wait_ms / total_tasks;

    if (!latencies_ms.empty()) {
        res.p50_latency_ms = latencies_ms[static_cast<size_t>(latencies_ms.size() * 0.50)];
        res.p95_latency_ms = latencies_ms[static_cast<size_t>(latencies_ms.size() * 0.95)];
        res.p99_latency_ms = latencies_ms[static_cast<size_t>(latencies_ms.size() * 0.99)];
        res.max_latency_ms = latencies_ms.back();
    }

    return res;
}

}  // namespace atp
