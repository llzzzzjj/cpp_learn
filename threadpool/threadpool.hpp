#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// 任务队列保存无参数、无返回值的可调用对象。
using Task = std::function<void()>;

class ThreadPool {
public:
    // thread_count 是工作线程数，max_queue_size 只限制等待执行的任务。
    explicit ThreadPool(
        std::size_t thread_count,
        std::size_t max_queue_size = 64
    );

    ~ThreadPool();

    //禁止拷贝构造、拷贝赋值
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 返回 true 表示入队成功，任务可能稍后才执行。
    bool submit(Task task);
    // 停止接收新任务，等已接收的任务处理完并回收工作线程。
    void shutdown();

private:
    void worker_loop();

    std::queue<Task> tasks_;    //等待执行的任务队列
    // 保护任务队列和停止标记。
    std::mutex mtx_;
    // 工作线程用它等待新任务或停止通知。
    std::condition_variable cv_;
    // 持锁访问。置为 true 后拒绝新任务，已有任务继续处理。
    bool stopping_ = false;
    // 等待队列容量，正在执行的任务已离开队列。
    const std::size_t max_queue_size_;
    std::vector<std::thread> workers_; //工作线程
};
