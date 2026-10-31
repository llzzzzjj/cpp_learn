#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

using Task = std::function<void()>;

class ThreadPool {
public:
    explicit ThreadPool(
        std::size_t thread_count,
        std::size_t max_queue_size = 64
    );

    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    bool submit(Task task);
    void shutdown();

private:
    void worker_loop();

    std::queue<Task> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stopping_ = false;
    const std::size_t max_queue_size_;
    std::vector<std::thread> workers_;
};