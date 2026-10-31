#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <functional>
#include <utility>
#include <cstddef>
#include <vector>
#include <stdexcept>
#include <exception>
#include <atomic>

using Task = std::function<void()>;

class ThreadPool{
public:
    explicit ThreadPool(
        std::size_t thread_count,
        std::size_t max_queue_size = 64
    );
    ~ThreadPool();

    //禁止拷贝
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    bool submit(Task task){
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

private:
    void worker_loop() {
        while (true) {
            Task task;

            {
                std::unique_lock<std::mutex>u_lock(mtx_);

                // 等待“队列非空，或者线程池开始停止”
                cv_.wait(u_lock,[this]{
                    return !tasks_.empty() || stopping_;
                });

                if(tasks_.empty() && stopping_){
                    break;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }

            // // TODO 9：在锁外执行 task
            // task();
            try {
                // TODO 1：执行当前任务
                task();
            } catch (const std::exception& e) {
                // TODO 2：向 std::cerr 输出错误信息 e.what()
                std::cerr<<e.what()<<std::endl;
            } catch (...) {
                // TODO 3：输出“任务发生未知异常”
                std::cerr<<"任务发生未知异常\n";
            }
        }
    }
private:
    std::queue<Task> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stopping_ = false;

    const std::size_t max_queue_size_;

    std::vector<std::thread> workers_;
};

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

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex>lg_lock(mtx_);
        stopping_ = true;
    }

    cv_.notify_all();
    for (auto& worker : workers_) {
        // 如果 worker.joinable()，调用 worker.join()。
        if(worker.joinable()){
            worker.join();
        }
    }
}
int main() {
    
    std::atomic<int>completed{0};

    {
        ThreadPool pool(2);
        pool.submit([]{
            throw std::runtime_error("模拟任务失败");
        });
        for (int id = 0; id < 5; ++id) {
            pool.submit([&completed] {
                ++completed;
            });
        }
    }
    try{
        ThreadPool pool(2,0);
    }catch (const std::invalid_argument& e){
        std::cout << "捕获参数失败：" <<e.what()<<'\n';
    }

    std::cout << "正常任务完成数："
              << completed.load() << '\n';

    return completed.load() == 5 ? 0 : 1;
    // return 0;
}