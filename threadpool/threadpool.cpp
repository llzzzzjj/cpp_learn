#include "threadpool.hpp"

#include <iostream>
#include <utility>
#include <stdexcept>
#include <exception>

ThreadPool::ThreadPool(
    std::size_t thread_count, 
    std::size_t max_queue_size
    ) : max_queue_size_(max_queue_size){
    if (thread_count == 0 || max_queue_size == 0) {
        throw std::invalid_argument("线程数量和队列容量都必须大于 0");
    }

    workers_.reserve(thread_count);//提前预留容纳线程对象的空间

    try {
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] {
                worker_loop();
            });
        }
    } catch (...) {
        // 如果中途创建线程失败，先结束已经创建的线程
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopping_ = true;
        }

        cv_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

// 提交者持锁完成容量检查和入队，解锁后唤醒一个工作线程。
bool ThreadPool::submit(Task task){
    // 拒绝没有保存任何函数的空任务
    if (!task) {
        return false;
    }

    {
        std::lock_guard<std::mutex>lg_lock(mtx_);
        if(stopping_){
            return false;
        }
        if(tasks_.size() >= max_queue_size_){
            return false;
        }
        tasks_.push(std::move(task));
    }

    cv_.notify_one();
    return true;
}
// 工作线程持锁取任务，解锁后执行，避免长任务占用队列锁。
void ThreadPool::worker_loop() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex>u_lock(mtx_);

            // 等待“队列非空，或者线程池开始停止”
            cv_.wait(u_lock,[this]{
                return !tasks_.empty() || stopping_;
            });

            // 开始停止且任务已取完时，这个工作线程才退出。
            if(tasks_.empty() && stopping_){
                break;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // 在锁外执行 task
        // task();
        try {
            // 执行当前任务
            task();
        } catch (const std::exception& e) {
            // 向 std::cerr 输出错误信息 e.what()
            std::cerr<<e.what()<<std::endl;
        } catch (...) {
            // 输出“任务发生未知异常”
            std::cerr<<"任务发生未知异常\n";
        }
    }
}

// 关闭提交入口，唤醒等待线程，并等它们处理完已有任务。
void ThreadPool::shutdown() {
    {
        // 持有 mtx_，设置 stopping_ = true。
        std::lock_guard<std::mutex>lock(mtx_);
        stopping_ = true;
    }

    // 通知所有等待的工作线程。
    cv_.notify_all();
    // 遍历 workers_，对 joinable 的线程调用 join()。
    for(auto &w : workers_){
        if(w.joinable())   
            w.join();
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

