#include "adaptive_controller.h"
#include "metrics.h"
#include "thread_pool.h"
#include "workloads.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct TrialStats {
    double median_wall_time = 0.0;
    double min_wall_time = 0.0;
    double max_wall_time = 0.0;
    double median_throughput = 0.0;
    double median_avg_latency_ms = 0.0;
    double median_p99_latency_ms = 0.0;
    double median_avg_queue_wait_ms = 0.0;
    size_t median_worker_changes = 0;
};

double calculate_median(std::vector<double> vals) {
    if (vals.empty()) return 0.0;
    std::sort(vals.begin(), vals.end());
    size_t mid = vals.size() / 2;
    if (vals.size() % 2 == 0) {
        return (vals[mid - 1] + vals[mid]) / 2.0;
    }
    return vals[mid];
}

TrialStats aggregate_trials(const std::vector<atp::BenchmarkResult>& trials) {
    TrialStats stats;
    if (trials.empty()) return stats;

    std::vector<double> wall_times;
    std::vector<double> throughs;
    std::vector<double> avg_lats;
    std::vector<double> p99_lats;
    std::vector<double> wait_times;
    std::vector<double> worker_changes;

    for (const auto& t : trials) {
        wall_times.push_back(t.wall_time_sec);
        throughs.push_back(t.throughput);
        avg_lats.push_back(t.avg_latency_ms);
        p99_lats.push_back(t.p99_latency_ms);
        wait_times.push_back(t.avg_queue_wait_ms);
        worker_changes.push_back(static_cast<double>(t.worker_changes));
    }

    stats.median_wall_time = calculate_median(wall_times);
    stats.min_wall_time = *std::min_element(wall_times.begin(), wall_times.end());
    stats.max_wall_time = *std::max_element(wall_times.begin(), wall_times.end());
    stats.median_throughput = calculate_median(throughs);
    stats.median_avg_latency_ms = calculate_median(avg_lats);
    stats.median_p99_latency_ms = calculate_median(p99_lats);
    stats.median_avg_queue_wait_ms = calculate_median(wait_times);
    stats.median_worker_changes = static_cast<size_t>(calculate_median(worker_changes));

    return stats;
}

void append_csv(const std::string& csv_file, const atp::BenchmarkResult& r, size_t trial_num) {
    if (csv_file.empty()) return;

    bool file_exists = false;
    {
        std::ifstream check(csv_file);
        file_exists = check.good();
    }

    std::ofstream out(csv_file, std::ios::app);
    if (!out.is_open()) return;

    if (!file_exists) {
        out << "mode,workload,workers,trial,tasks,wall_time_sec,throughput,avg_lat_ms,"
               "p50_lat_ms,p95_lat_ms,p99_lat_ms,max_lat_ms,avg_wait_ms,worker_changes\n";
    }

    out << r.pool_mode << ","
        << r.workload_name << ","
        << r.configured_workers << ","
        << trial_num << ","
        << r.total_tasks << ","
        << std::fixed << std::setprecision(4)
        << r.wall_time_sec << ","
        << std::setprecision(1)
        << r.throughput << ","
        << std::setprecision(3)
        << r.avg_latency_ms << ","
        << r.p50_latency_ms << ","
        << r.p95_latency_ms << ","
        << r.p99_latency_ms << ","
        << r.max_latency_ms << ","
        << r.avg_queue_wait_ms << ","
        << r.worker_changes << "\n";
}

atp::BenchmarkResult run_single_fixed_trial(atp::WorkloadType workload, size_t workers, double scale = 1.0) {
    // Fixed sweeps must honor the requested worker count, including values above
    // ThreadPool's default dynamic-pool ceiling of 32.
    atp::ThreadPool pool(workers, 1, workers);
    atp::BenchmarkResult res = atp::execute_workload(workload, pool, "fixed", scale);
    pool.shutdown();
    return res;
}

atp::BenchmarkResult run_single_adaptive_trial(atp::WorkloadType workload,
                                             size_t initial_workers,
                                             size_t min_workers,
                                             size_t max_workers,
                                             std::chrono::milliseconds window_ms,
                                             double scale = 1.0) {
    atp::ThreadPool pool(initial_workers, min_workers, max_workers);
    atp::ControllerConfig cfg;
    cfg.window_interval = window_ms;
    cfg.min_workers = min_workers;
    cfg.max_workers = max_workers;
    cfg.epsilon = 0.02;
    cfg.cooldown_windows = 2;
    cfg.alpha = 0.35;
    cfg.max_step = 4;

    atp::AdaptiveController controller(pool, cfg);
    controller.start();

    atp::BenchmarkResult res = atp::execute_workload(workload, pool, "adaptive", scale);

    controller.stop();
    res.worker_changes = controller.resize_count();
    res.worker_history = controller.get_worker_history();
    pool.shutdown();
    return res;
}

void print_table_header() {
    std::cout << "\n"
              << std::left << std::setw(16) << "Mode"
              << std::setw(18) << "Workload"
              << std::right << std::setw(12) << "Workers"
              << std::setw(22) << "Time (s)"
              << std::setw(18) << "Throughput (t/s)"
              << std::setw(16) << "Avg Lat (ms)"
              << std::setw(16) << "P99 Lat (ms)"
              << std::setw(16) << "Wait (ms)"
              << std::setw(10) << "Resizes"
              << "\n"
              << std::string(132, '-') << "\n";
}

void print_table_row(const std::string& mode,
                     const std::string& workload,
                     const std::string& workers,
                     const TrialStats& stats) {
    std::ostringstream time_str;
    time_str << std::fixed << std::setprecision(3) << stats.median_wall_time
             << " [" << std::setprecision(2) << stats.min_wall_time << "-" << stats.max_wall_time << "]";

    std::cout << std::left << std::setw(16) << mode
              << std::setw(18) << workload
              << std::right << std::setw(12) << workers
              << std::setw(22) << time_str.str()
              << std::fixed << std::setprecision(1) << std::setw(18) << stats.median_throughput
              << std::fixed << std::setprecision(2) << std::setw(16) << stats.median_avg_latency_ms
              << std::fixed << std::setprecision(2) << std::setw(16) << stats.median_p99_latency_ms
              << std::fixed << std::setprecision(2) << std::setw(16) << stats.median_avg_queue_wait_ms
              << std::setw(10) << stats.median_worker_changes
              << "\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string mode = "compare";
    std::string workload_filter = "all";
    size_t workers = 4;
    size_t trials = 5;
    size_t window_ms = 100;
    size_t min_workers = 1;
    size_t max_workers = 24;
    size_t initial_workers = 4;
    double scale = 1.0;
    std::string csv_file = "benchmark_results.csv";
    std::vector<size_t> custom_fixed_configs;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--workload" && i + 1 < argc) {
            workload_filter = argv[++i];
        } else if (arg == "--workers" && i + 1 < argc) {
            workers = std::stoul(argv[++i]);
        } else if (arg == "--trials" && i + 1 < argc) {
            trials = std::stoul(argv[++i]);
        } else if (arg == "--window-ms" && i + 1 < argc) {
            window_ms = std::stoul(argv[++i]);
        } else if (arg == "--min-workers" && i + 1 < argc) {
            min_workers = std::stoul(argv[++i]);
        } else if (arg == "--max-workers" && i + 1 < argc) {
            max_workers = std::stoul(argv[++i]);
        } else if (arg == "--initial-workers" && i + 1 < argc) {
            initial_workers = std::stoul(argv[++i]);
        } else if (arg == "--workers-list" && i + 1 < argc) {
            std::string list_str = argv[++i];
            std::stringstream ss(list_str);
            std::string item;
            custom_fixed_configs.clear();
            while (std::getline(ss, item, ',')) {
                if (!item.empty()) {
                    custom_fixed_configs.push_back(std::stoul(item));
                }
            }
        } else if (arg == "--scale" && i + 1 < argc) {
            scale = std::stod(argv[++i]);
        } else if (arg == "--csv" && i + 1 < argc) {
            csv_file = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Adaptive Thread Pool Benchmark Harness\n"
                      << "Usage: ./benchmark [options]\n\n"
                      << "Options:\n"
                      << "  --mode <fixed|fixed-sweep|adaptive|compare>  (default: compare)\n"
                      << "  --workload <cpu|wait|mixed|bursty|phase|all> (default: all)\n"
                      << "  --workers <N>                                Fixed worker count (default: 4)\n"
                      << "  --workers-list <N,N,...>                     Comma-separated fixed workers to compare (e.g. 4,8,10,16,24,32)\n"
                      << "  --trials <N>                                 Number of trials per run (default: 5)\n"
                      << "  --window-ms <N>                              Controller window interval (default: 100)\n"
                      << "  --min-workers <N>                            Adaptive pool min workers (default: 1)\n"
                      << "  --max-workers <N>                            Adaptive pool max workers (default: 24)\n"
                      << "  --initial-workers <N>                        Adaptive pool starting workers (default: 4)\n"
                      << "  --scale <factor>                             Workload task multiplier (default: 1.0)\n"
                      << "  --csv <filename>                             CSV output destination (default: benchmark_results.csv)\n";
            return 0;
        }
    }

    std::vector<atp::WorkloadType> target_workloads;
    if (workload_filter == "cpu") {
        target_workloads = {atp::WorkloadType::kCpuBound};
    } else if (workload_filter == "wait") {
        target_workloads = {atp::WorkloadType::kWaitHeavy};
    } else if (workload_filter == "mixed") {
        target_workloads = {atp::WorkloadType::kMixed};
    } else if (workload_filter == "bursty") {
        target_workloads = {atp::WorkloadType::kBursty};
    } else if (workload_filter == "phase") {
        target_workloads = {atp::WorkloadType::kPhaseChanging};
    } else {
        target_workloads = {
            atp::WorkloadType::kCpuBound,
            atp::WorkloadType::kWaitHeavy,
            atp::WorkloadType::kMixed,
            atp::WorkloadType::kBursty,
            atp::WorkloadType::kPhaseChanging
        };
    }

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;

    std::vector<size_t> fixed_configs = {1, 2, 4, 8, hw, hw * 2};
    if (!custom_fixed_configs.empty()) {
        fixed_configs = custom_fixed_configs;
    }
    std::sort(fixed_configs.begin(), fixed_configs.end());
    fixed_configs.erase(std::unique(fixed_configs.begin(), fixed_configs.end()), fixed_configs.end());

    std::cout << "========================================================================================\n"
              << "       ADAPTIVE THREAD POOL: SYSTEM BENCHMARK SUITE                                     \n"
              << "========================================================================================\n"
              << "Hardware concurrency detected: " << hw << " cores\n"
              << "Mode: " << mode << " | Trials per config: " << trials << " (reporting medians)\n"
              << "Warm-up phase: DISABLED (per benchmark specification)\n"
              << "Writing detailed trial data to: " << csv_file << "\n";

    print_table_header();

    for (auto wl : target_workloads) {
        std::string wl_name = atp::to_string(wl);

        if (mode == "fixed") {
            std::vector<atp::BenchmarkResult> trial_results;
            for (size_t t = 1; t <= trials; ++t) {
                auto res = run_single_fixed_trial(wl, workers, scale);
                append_csv(csv_file, res, t);
                trial_results.push_back(res);
            }
            TrialStats stats = aggregate_trials(trial_results);
            print_table_row("Fixed", wl_name, std::to_string(workers), stats);
        } else if (mode == "fixed-sweep") {
            for (size_t w : fixed_configs) {
                std::vector<atp::BenchmarkResult> trial_results;
                for (size_t t = 1; t <= trials; ++t) {
                    auto res = run_single_fixed_trial(wl, w, scale);
                    append_csv(csv_file, res, t);
                    trial_results.push_back(res);
                }
                TrialStats stats = aggregate_trials(trial_results);
                print_table_row("Fixed", wl_name, std::to_string(w), stats);
            }
        } else if (mode == "adaptive") {
            std::vector<atp::BenchmarkResult> trial_results;
            for (size_t t = 1; t <= trials; ++t) {
                auto res = run_single_adaptive_trial(wl, initial_workers, min_workers, max_workers,
                                                    std::chrono::milliseconds(window_ms), scale);
                append_csv(csv_file, res, t);
                trial_results.push_back(res);
            }
            TrialStats stats = aggregate_trials(trial_results);
            std::string worker_label = "[" + std::to_string(min_workers) + ".." + std::to_string(max_workers) + "]";
            print_table_row("Adaptive (HC)", wl_name, worker_label, stats);
        } else if (mode == "compare") {
            // Run fixed configs
            for (size_t w : fixed_configs) {
                std::vector<atp::BenchmarkResult> trial_results;
                for (size_t t = 1; t <= trials; ++t) {
                    auto res = run_single_fixed_trial(wl, w, scale);
                    append_csv(csv_file, res, t);
                    trial_results.push_back(res);
                }
                TrialStats stats = aggregate_trials(trial_results);
                print_table_row("Fixed", wl_name, std::to_string(w), stats);
            }

            // Run adaptive
            std::vector<atp::BenchmarkResult> adapt_results;
            for (size_t t = 1; t <= trials; ++t) {
                auto res = run_single_adaptive_trial(wl, initial_workers, min_workers, max_workers,
                                                    std::chrono::milliseconds(window_ms), scale);
                append_csv(csv_file, res, t);
                adapt_results.push_back(res);
            }
            TrialStats adapt_stats = aggregate_trials(adapt_results);
            std::string worker_label = "[" + std::to_string(min_workers) + ".." + std::to_string(max_workers) + "]";
            print_table_row("Adaptive (HC)", wl_name, worker_label, adapt_stats);
            std::cout << std::string(127, '-') << "\n";
        }
    }

    std::cout << "\nBenchmark complete. CSV logs stored in: " << csv_file << "\n" << std::endl;
    return 0;
}
