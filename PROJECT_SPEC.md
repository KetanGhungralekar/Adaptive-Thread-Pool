# Adaptive Thread Pool: Fixed-Size Baseline vs Hill-Climbing Auto-Tuning

## 1. Project Overview

Build a generic C++ thread pool and investigate whether the number of worker threads can be automatically tuned according to the current workload.

The project will be developed in two major stages:

1. **Fixed-size thread pool** — establish a conventional baseline and benchmark different worker counts.
2. **Hill-climbing adaptive thread pool** — automatically experiment with different worker counts and move toward a configuration that provides better performance.

The project is primarily a **systems/concurrency experiment**, not an attempt to invent a new thread-pool implementation.

The central question is:

> **Can a thread pool automatically find a better worker count for a changing workload instead of requiring the number of workers to be chosen manually?**

---

# 2. Motivation

A conventional thread pool requires the developer to choose a worker count:

```text
Fixed Pool

Tasks
  |
  v
Queue
  |
  +---- Worker 1
  +---- Worker 2
  +---- Worker 3
  +---- Worker 4
```

The optimal number of workers may depend on the workload.

For example:

- CPU-bound tasks may benefit from a worker count close to the available CPU resources.
- Waiting-heavy tasks may benefit from more workers.
- Too many workers can introduce scheduling/context-switching overhead.
- A burst of incoming tasks may temporarily require more concurrency.
- A low workload may make a large worker pool waste resources.

Therefore, instead of assuming that one worker count is optimal for all workloads, this project will experimentally investigate whether the system can **measure its performance and adjust the worker count automatically**.

---

# 3. Project Goals

The final system should:

1. Implement a correct fixed-size C++ thread pool.
2. Provide a configurable number of workers.
3. Create a benchmark framework for different workloads.
4. Establish performance baselines for different fixed worker counts.
5. Implement a hill-climbing controller for worker-count selection.
6. Allow the adaptive pool to change its worker count while processing tasks.
7. Compare fixed-size and adaptive approaches.
8. Measure actual performance rather than assuming adaptive scaling is better.
9. Identify workloads where hill climbing helps and workloads where it does not.
10. Produce results that can be explained clearly in an SDE interview.

---

# 4. Scope

## Required

The project must include:

- C++17 or later
- Generic task submission
- Thread-safe task queue
- Worker threads
- Mutexes
- Condition variables
- Graceful shutdown
- Configurable worker count
- Fixed-size thread pool
- Workload generator
- Benchmarking framework
- Performance measurement
- Hill-climbing controller
- Dynamic worker creation
- Dynamic worker retirement
- Fixed vs adaptive comparison

## Not Required

Do not initially implement:

- networking
- database integration
- GUI
- distributed execution
- external thread-pool libraries
- machine learning
- reinforcement learning
- complex lock-free data structures
- production-grade application-server functionality

The entire project should remain realistic for one person to complete in approximately 1–2 days.

---

# 5. Phase 1 — Fixed-Size Thread Pool

First implement a conventional thread pool.

Architecture:

```text
                    Application
                        |
                        | submit(task)
                        v
                 +--------------+
                 |  Task Queue  |
                 +------+-------+
                        |
             +----------+----------+
             |          |          |
             v          v          v
           Worker 1  Worker 2  Worker 3
```

Workers repeatedly:

```cpp
while (running) {
    wait_for_task();
    task = get_task();
    execute(task);
}
```

The worker count must be configurable.

For example:

```bash
./benchmark --workers 2
./benchmark --workers 4
./benchmark --workers 8
./benchmark --workers 16
```

The first phase should contain **no adaptive behavior**.

This provides a clean baseline.

---

# 6. Phase 2 — Workload Generator

Create a benchmark program capable of generating controlled workloads.

The pool itself should remain generic.

The benchmark should submit arbitrary tasks to the pool.

## Workload A — CPU-bound

Examples:

- prime number calculations
- computational loops
- matrix operations
- hashing

The exact computation should be deterministic enough to make comparisons meaningful.

## Workload B — Waiting-heavy

Simulate tasks that spend significant time waiting.

For example:

```cpp
std::this_thread::sleep_for(...);
```

The goal is to approximate workloads where workers spend part of their lifetime waiting rather than continuously consuming CPU.

## Workload C — Mixed

Combine CPU-heavy and waiting tasks.

For example:

```text
CPU
WAIT
CPU
CPU
WAIT
...
```

## Workload D — Bursty

Create changing demand over time.

Example:

```text
100 tasks
     |
     | quiet
     |
1000 tasks
     |
     | quiet
     |
500 tasks
```

This workload is particularly important because the optimal worker count may change over time.

---

# 7. Phase 3 — Benchmark Fixed Worker Counts

Before implementing adaptive behavior, benchmark several fixed worker counts.

For example:

```text
2 workers
4 workers
8 workers
16 workers
```

For each workload, measure:

- total execution time
- throughput
- average task latency
- maximum task latency
- queue waiting time if practical
- CPU utilization if practical
- worker count

Example benchmark table:

```text
Workload: CPU-bound

Workers      Execution Time      Throughput
2            ...                 ...
4            ...                 ...
8            ...                 ...
16           ...                 ...
```

The actual values must come from running the benchmark.

Do not fabricate results.

---

# 8. Purpose of Phase 3

The fixed-size experiments should answer:

> **Does the best worker count remain the same across different workloads?**

For example, it is possible that the experiments show:

```text
CPU-bound       → best around 4–8 workers
Waiting-heavy   → best around 12–16 workers
Mixed           → somewhere in between
```

The exact results are unknown until the experiments are performed.

This observation will motivate the adaptive controller.

---

# 9. Phase 4 — Hill-Climbing Controller

Implement a hill-climbing algorithm that treats:

> **worker count = parameter being optimized**

Instead of manually choosing:

```text
workers = 8
```

the adaptive system periodically evaluates whether changing the worker count improves performance.

Conceptually:

```text
             Current configuration
                    |
                    v
              K workers
                    |
             measure score
                    |
                    v
             Try K + 1
                    |
             measure score
                    |
              /-----------\
             /             \
        improvement      worse
            |               |
            v               v
       keep K + 1        revert K
                            |
                            v
                         Try K - 1
```

The exact hill-climbing strategy should be designed after analyzing the benchmark requirements.

---

# 10. Hill-Climbing Design Requirements

The planner should consider several reasonable variants before implementation.

For example:

### Variant A — Simple Neighbor Search

At the current worker count `K`:

1. Measure performance at `K`.
2. Temporarily move to `K + 1`.
3. Measure performance.
4. If performance improves, keep `K + 1`.
5. Otherwise try `K - 1`.
6. Keep the better configuration.

### Variant B — Directional Hill Climbing

If increasing workers improves performance:

```text
K → K+1 → K+2 → ...
```

Continue in that direction until performance stops improving.

Then reverse direction if appropriate.

### Variant C — Windowed / Smoothed Hill Climbing

Use a moving average of recent performance measurements rather than reacting to a single measurement.

This reduces sensitivity to noisy workloads.

The planner should evaluate these alternatives and recommend one based on:

- implementation complexity
- stability
- measurement overhead
- responsiveness
- interview value
- suitability for a 1–2 day project

Do not implement all variants unless there is a clear reason.

---

# 11. Performance Objective

Do not automatically optimize throughput alone.

The planner should consider how to define the hill-climbing objective.

Possible signals include:

- throughput
- average latency
- maximum latency
- queue waiting time
- CPU utilization
- worker/resource overhead

A simple initial objective could prioritize throughput while penalizing excessive latency or worker count.

For example, conceptually:

```text
score =
    throughput
    - latency_penalty
    - resource_penalty
```

The exact scoring function should be proposed during the planning stage.

Keep the first implementation simple enough to understand and debug.

---

# 12. Measurement Window

The adaptive controller must not change the worker count after every individual task.

Instead, it should observe performance over a measurement window.

For example:

```text
Current configuration
       |
       v
Collect measurements for N tasks / time interval
       |
       v
Calculate performance score
       |
       v
Try neighboring worker count
       |
       v
Collect another measurement window
       |
       v
Compare
```

The window size should be configurable.

The project should investigate whether too-small windows cause noisy decisions.

---

# 13. Handling Workload Changes

The adaptive system must be tested with workloads whose characteristics change over time.

For example:

```text
Phase 1: CPU-bound
        ↓
Phase 2: waiting-heavy
        ↓
Phase 3: CPU-bound
```

or:

```text
Low workload
     ↓
Large burst
     ↓
Low workload
     ↓
Large burst
```

The goal is to determine whether the hill-climbing controller can move toward a better worker count after the workload changes.

---

# 14. Worker Scaling Constraints

The adaptive pool should have:

```text
MIN_WORKERS
MAX_WORKERS
```

For example:

```text
MIN_WORKERS = 1
MAX_WORKERS = 16
```

These values should be configurable.

The controller must never exceed these limits.

Worker creation and retirement must be implemented safely.

---

# 15. Stability Requirements

The adaptive algorithm should avoid excessive oscillation.

For example, this is undesirable:

```text
4 → 5 → 4 → 5 → 4 → 5
```

The implementation should consider:

- measurement windows
- moving averages
- minimum improvement thresholds
- cooldown periods
- hysteresis
- minimum time before reversing direction

The planner should recommend a simple mechanism appropriate for the project scope.

---

# 16. Benchmark Comparison

The final benchmark should compare:

### Fixed-size pools

```text
Fixed 2
Fixed 4
Fixed 8
Fixed 16
```

### Adaptive pool

```text
Hill-climbing
```

Across:

- CPU-bound workload
- waiting-heavy workload
- mixed workload
- bursty workload
- changing workload

---

# 17. Metrics

Collect as many of the following as practical:

### Primary metrics

- total execution time
- throughput
- average task latency

### Secondary metrics

- maximum task latency
- queue waiting time
- CPU utilization
- peak worker count
- average worker count
- number of worker-count changes
- scaling overhead

The benchmark should make it possible to see not only **whether the adaptive pool is faster**, but also **why**.

---

# 18. Expected Outcome

Do not assume the adaptive pool will outperform every fixed-size configuration.

The project should explicitly investigate:

> When does hill climbing help?

and:

> When does hill climbing introduce enough measurement/scaling overhead that a fixed pool is better?

Possible outcomes include:

```text
Adaptive better
Adaptive similar
Adaptive worse
```

All three are valid results.

The final README should explain the actual observations.

---

# 19. Correctness and Concurrency

The implementation must avoid:

- data races
- deadlocks
- lost tasks
- duplicate task execution
- unsafe worker termination
- tasks being abandoned during shutdown

Test at minimum:

1. Empty queue.
2. One task.
3. Many tasks.
4. More tasks than workers.
5. Concurrent task submission.
6. Shutdown with pending tasks.
7. Worker creation.
8. Worker retirement.
9. Minimum worker limit.
10. Maximum worker limit.
11. Repeated adaptive scaling.

---

# 20. Technology

Use:

**C++17 or later**

Prefer:

```text
std::thread
std::mutex
std::condition_variable
std::atomic
std::function
std::future
std::chrono
std::queue / std::deque
```

Use CMake for building.

Avoid external concurrency libraries.

---

# 21. Suggested Repository Structure

The exact structure may be adjusted by the planner, but aim for something similar to:

```text
adaptive-thread-pool/
│
├── include/
│   ├── thread_pool.h
│   ├── task_queue.h
│   └── adaptive_controller.h
│
├── src/
│   ├── thread_pool.cpp
│   ├── task_queue.cpp
│   └── adaptive_controller.cpp
│
├── benchmarks/
│   ├── benchmark.cpp
│   ├── workloads.cpp
│   └── metrics.cpp
│
├── tests/
│   └── ...
│
├── CMakeLists.txt
├── README.md
└── PROJECT_SPEC.md
```

Keep the structure simple if fewer files are sufficient.

---

# 22. Development Sequence

The implementation must follow this sequence.

## Milestone 1

Implement the fixed-size thread pool.

Do not implement adaptive behavior.

## Milestone 2

Build the workload generator and benchmark framework.

## Milestone 3

Benchmark fixed worker counts and verify that the benchmark produces sensible results.

## Milestone 4

Design and implement the hill-climbing controller.

## Milestone 5

Integrate the controller with the thread pool.

## Milestone 6

Test adaptive behavior under changing workloads.

## Milestone 7

Run the final fixed-vs-adaptive experiments.

## Milestone 8

Analyze and document the results.

---

# 23. Interview Narrative

The final project should support this story:

### Situation

> Fixed-size thread pools require the worker count to be selected manually, but different workloads can have different optimal concurrency levels.

### Task

> I wanted to investigate whether the system could automatically tune its worker count based on observed performance.

### Action

> I first implemented a conventional fixed-size thread pool and benchmarked different worker counts across CPU-bound, waiting-heavy, mixed, and bursty workloads. I then implemented a hill-climbing controller that treated worker count as an optimization parameter and automatically experimented with neighboring configurations.

### Result

> I compared the adaptive system against fixed-size pools using throughput, latency, and resource usage, and analyzed where adaptive tuning helped and where its measurement overhead or workload changes made it less effective.

The actual results must be based on experiments and must not be fabricated.

---

# 24. Engineering Principle

The project should follow:

```text
Fixed implementation
        ↓
Benchmark
        ↓
Observation
        ↓
Problem identified
        ↓
Hill-climbing design
        ↓
Implementation
        ↓
Benchmark
        ↓
Comparison
        ↓
Tradeoff analysis
```

Do not start by assuming that hill climbing is the correct solution.

The purpose of the project is to **experimentally investigate whether it is useful**.