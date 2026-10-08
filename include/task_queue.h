#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace atp {

/// Result of a timed pop operation on the task queue.
enum class PopResult {
    kSuccess,  // A task was retrieved
    kTimeout,  // Timed out waiting for a task
    kClosed    // Queue is closed and empty
};

/// Thread-safe MPMC task queue backed by a mutex-guarded deque.
///
/// Supports blocking timed pops, blocking untimed pops, and cooperative shutdown via close().
/// Workers block in pop() / try_pop() until a task arrives, the timeout expires,
/// or the queue is closed.
class TaskQueue {
public:
    TaskQueue() = default;
    ~TaskQueue() = default;

    // Non-copyable, non-movable
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    /// Push a task onto the queue. Returns false if the queue is closed.
    bool push(std::function<void()> task);

    /// Wait indefinitely for a task.
    /// Returns true if a task was retrieved.
    /// Returns false if the queue is closed AND empty (workers should exit).
    bool pop(std::function<void()>& task);

    /// Wait up to @p timeout for a task.
    ///  - kSuccess: @p task is populated.
    ///  - kTimeout: nothing available within the deadline.
    ///  - kClosed:  queue is closed AND empty — worker should exit.
    PopResult try_pop(std::function<void()>& task,
                      std::chrono::milliseconds timeout);

    /// Wake up all waiting consumers (e.g. during worker resizing).
    void notify_all();

    /// Close the queue. No further pushes are accepted.
    /// All waiting consumers are woken up.
    void close();

    /// Current number of enqueued tasks (informational only).
    size_t size() const;

    /// Whether close() has been called.
    bool is_closed() const;

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    bool closed_ = false;
};

}  // namespace atp
