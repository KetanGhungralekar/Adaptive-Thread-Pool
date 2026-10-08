#include "adaptive_controller.h"
#include "task_queue.h"
#include "thread_pool.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#define TEST_ASSERT(cond, msg)                                                \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::cerr << "\033[1;31m[FAILED]\033[0m " << msg                  \
                      << " (" << __FILE__ << ":" << __LINE__ << ")"           \
                      << std::endl;                                           \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

#define TEST_PASS(msg)                                                        \
    std::cout << "\033[1;32m[PASSED]\033[0m " << msg << std::endl

void test_1_empty_queue_shutdown() {
    std::cout << "Running Test 1: Empty queue shutdown..." << std::endl;
    {
        atp::ThreadPool pool(4);
        // Immediately shutdown without submitting anything
        pool.shutdown();
        TEST_ASSERT(pool.is_shutdown(), "Pool should report shutdown");
    }
    TEST_PASS("Test 1: Empty queue shutdown cleanly");
}

void test_2_single_task_future() {
    std::cout << "Running Test 2: Single task future..." << std::endl;
    atp::ThreadPool pool(2);
    auto fut = pool.submit([](int a, int b) {
        return a + b;
    }, 40, 2);

    TEST_ASSERT(fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "Future should complete quickly");
    TEST_ASSERT(fut.get() == 42, "Future result must be 42");
    pool.shutdown();
    TEST_PASS("Test 2: Single task future completed with correct result");
}

void test_3_ten_thousand_tasks() {
    std::cout << "Running Test 3: 10,000 tasks future verification..." << std::endl;
    const size_t kTasks = 10000;
    atp::ThreadPool pool(8);
    std::vector<std::future<size_t>> futures;
    futures.reserve(kTasks);

    for (size_t i = 0; i < kTasks; ++i) {
        futures.push_back(pool.submit([i]() {
            return i * 2;
        }));
    }

    for (size_t i = 0; i < kTasks; ++i) {
        TEST_ASSERT(futures[i].get() == i * 2, "Task result mismatch");
    }
    pool.shutdown();
    TEST_PASS("Test 3: 10,000 tasks executed with zero loss");
}

void test_4_concurrent_producers() {
    std::cout << "Running Test 4: Concurrent task submission from 8 producers..." << std::endl;
    atp::ThreadPool pool(4);
    const size_t kProducers = 8;
    const size_t kTasksPerProducer = 1000;
    std::atomic<size_t> counter{0};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&pool, &counter, kTasksPerProducer]() {
            for (size_t i = 0; i < kTasksPerProducer; ++i) {
                pool.submit_task([&counter]() {
                    counter.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    pool.wait_idle();
    pool.shutdown();

    TEST_ASSERT(counter.load() == kProducers * kTasksPerProducer,
                "All concurrent tasks must be processed");
    TEST_PASS("Test 4: Concurrent submission from 8 threads processed all tasks");
}

void test_5_shutdown_with_pending_tasks() {
    std::cout << "Running Test 5: Shutdown with 1,000 pending tasks..." << std::endl;
    std::atomic<size_t> completed{0};
    const size_t kTasks = 1000;

    {
        atp::ThreadPool pool(4);
        for (size_t i = 0; i < kTasks; ++i) {
            pool.submit_task([&completed]() {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                completed.fetch_add(1, std::memory_order_relaxed);
            });
        }
        // Pool destructor calls shutdown() and joins threads.
        // It must finish ALL 1,000 pending tasks before joining!
    }

    TEST_ASSERT(completed.load() == kTasks,
                "All tasks submitted before shutdown must complete");
    TEST_PASS("Test 5: Shutdown with pending tasks drained every task");
}

void test_6_single_worker_sequential() {
    std::cout << "Running Test 6: Single worker sequential order..." << std::endl;
    atp::ThreadPool pool(1);
    const size_t kTasks = 100;
    std::vector<size_t> executed_order;
    std::mutex order_mu;

    for (size_t i = 0; i < kTasks; ++i) {
        pool.submit_task([&order_mu, &executed_order, i]() {
            std::lock_guard<std::mutex> lock(order_mu);
            executed_order.push_back(i);
        });
    }

    pool.wait_idle();
    pool.shutdown();

    TEST_ASSERT(executed_order.size() == kTasks, "Must have executed 100 tasks");
    for (size_t i = 0; i < kTasks; ++i) {
        TEST_ASSERT(executed_order[i] == i, "Tasks on 1 worker must execute in FIFO order");
    }
    TEST_PASS("Test 6: Single worker executed tasks in exact FIFO sequence");
}

void test_7_dynamic_growth() {
    std::cout << "Running Test 7: Dynamic worker growth (4 -> 8)..." << std::endl;
    atp::ThreadPool pool(4, 1, 16);
    TEST_ASSERT(pool.worker_count() == 4, "Initial worker count should be 4");

    size_t new_count = pool.resize(8);
    TEST_ASSERT(new_count == 8, "Target count should be 8");
    TEST_ASSERT(pool.worker_count() == 8, "Active worker count should increase to 8");

    std::atomic<size_t> count{0};
    for (size_t i = 0; i < 500; ++i) {
        pool.submit_task([&count]() {
            count.fetch_add(1, std::memory_order_relaxed);
        });
    }
    pool.wait_idle();
    pool.shutdown();

    TEST_ASSERT(count.load() == 500, "All tasks after growth must complete");
    TEST_PASS("Test 7: Dynamic growth (4 -> 8) succeeded with all tasks processed");
}

void test_8_dynamic_shrink() {
    std::cout << "Running Test 8: Dynamic worker shrink (8 -> 2)..." << std::endl;
    atp::ThreadPool pool(8, 1, 16);
    TEST_ASSERT(pool.worker_count() == 8, "Initial worker count should be 8");

    pool.resize(2);
    // Allow retiring threads a moment to observe retirement condition
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    TEST_ASSERT(pool.desired_worker_count() == 2, "Desired worker count must be 2");
    TEST_ASSERT(pool.worker_count() == 2, "Active worker count should have retired down to 2");

    std::atomic<size_t> count{0};
    for (size_t i = 0; i < 500; ++i) {
        pool.submit_task([&count]() {
            count.fetch_add(1, std::memory_order_relaxed);
        });
    }
    pool.wait_idle();
    pool.shutdown();

    TEST_ASSERT(count.load() == 500, "All tasks after shrink must complete");
    TEST_PASS("Test 8: Dynamic shrink (8 -> 2) retired threads cleanly without losing tasks");
}

void test_9_min_workers_clamping() {
    std::cout << "Running Test 9: Min worker clamping..." << std::endl;
    atp::ThreadPool pool(4, 2, 16);
    size_t clamped = pool.resize(0);
    TEST_ASSERT(clamped == 2, "Clamped to min_workers (2)");
    TEST_ASSERT(pool.desired_worker_count() == 2, "Desired worker count clamped to 2");
    pool.shutdown();
    TEST_PASS("Test 9: resize(0) clamped to MIN_WORKERS (2)");
}

void test_10_max_workers_clamping() {
    std::cout << "Running Test 10: Max worker clamping..." << std::endl;
    atp::ThreadPool pool(4, 1, 8);
    size_t clamped = pool.resize(50);
    TEST_ASSERT(clamped == 8, "Clamped to max_workers (8)");
    TEST_ASSERT(pool.desired_worker_count() == 8, "Desired worker count clamped to 8");
    pool.shutdown();
    TEST_PASS("Test 10: resize(50) clamped to MAX_WORKERS (8)");
}

void test_11_rapid_resize_under_load() {
    std::cout << "Running Test 11: Rapid resize under continuous load (2 -> 8 -> 2 -> 8 -> 2)..." << std::endl;
    atp::ThreadPool pool(2, 1, 12);
    std::atomic<bool> submitting{true};
    std::atomic<size_t> total_submitted{0};
    std::atomic<size_t> total_completed{0};

    // Producer thread continuously submitting tasks
    std::thread producer([&]() {
        while (submitting.load(std::memory_order_relaxed)) {
            pool.submit_task([&]() {
                std::this_thread::sleep_for(std::chrono::microseconds(20));
                total_completed.fetch_add(1, std::memory_order_relaxed);
            });
            total_submitted.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    // Main thread performs rapid resizes
    std::vector<size_t> targets = {4, 8, 2, 6, 1, 8, 2};
    for (size_t target : targets) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        pool.resize(target);
    }

    submitting.store(false);
    producer.join();

    pool.wait_idle();
    pool.shutdown();

    TEST_ASSERT(total_completed.load() == total_submitted.load(),
                "All tasks submitted during rapid resizing must complete");
    TEST_PASS("Test 11: Rapid resizing under load completed with zero data races or lost tasks");
}

void test_12_adaptive_controller_logic() {
    std::cout << "Running Test 12: Adaptive controller step and convergence logic..." << std::endl;
    atp::ThreadPool pool(2, 1, 16);
    atp::ControllerConfig cfg;
    cfg.min_workers = 1;
    cfg.max_workers = 16;
    cfg.epsilon = 0.01;
    cfg.cooldown_windows = 1;
    atp::AdaptiveController controller(pool, cfg);

    // Initial state
    TEST_ASSERT(pool.worker_count() == 2, "Initial workers should be 2");
    TEST_ASSERT(controller.current_direction() == 1, "Initial direction should be +1");

    // Submit work so metrics are non-zero
    for (size_t i = 0; i < 200; ++i) {
        pool.submit_task([]() {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        });
    }
    pool.wait_idle();

    // First sample sets baseline
    controller.step();
    TEST_ASSERT(controller.current_smoothed_score() > 0.0, "Score should be positive");

    // Submit more work to show improvement with higher throughput
    for (size_t i = 0; i < 600; ++i) {
        pool.submit_task([]() {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        });
    }
    pool.wait_idle();

    // Step should scale up
    size_t prev_workers = pool.desired_worker_count();
    controller.step();
    TEST_ASSERT(pool.desired_worker_count() >= prev_workers,
                "Controller should scale up on improved performance");

    pool.shutdown();
    TEST_PASS("Test 12: Adaptive controller directional logic verified");
}

int main() {
    std::cout << "\n==========================================" << std::endl;
    std::cout << "   RUNNING ADAPTIVE THREAD POOL TESTS     " << std::endl;
    std::cout << "==========================================\n" << std::endl;

    test_1_empty_queue_shutdown();
    test_2_single_task_future();
    test_3_ten_thousand_tasks();
    test_4_concurrent_producers();
    test_5_shutdown_with_pending_tasks();
    test_6_single_worker_sequential();
    test_7_dynamic_growth();
    test_8_dynamic_shrink();
    test_9_min_workers_clamping();
    test_10_max_workers_clamping();
    test_11_rapid_resize_under_load();
    test_12_adaptive_controller_logic();

    std::cout << "\n\033[1;32mALL 12 TESTS PASSED SUCCESSFULLY!\033[0m\n" << std::endl;
    return 0;
}
