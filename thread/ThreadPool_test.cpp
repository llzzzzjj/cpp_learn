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

using Task = std::function<void()>;

class ThreadPool{
public:
    explicit ThreadPool(std::size_t thread_count);
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
            // TODO 1：使用 lock_guard 锁住 mtx_
            std::lock_guard<std::mutex>lg_lock(mtx_);
            // TODO 2：如果 stopping_ 为真，返回 false
            if(stopping_){
                return false;
            }
            // TODO 3：将 task 移动到 tasks_ 中
            tasks_.push(std::move(task));
        }

        // TODO 4：通知一个等待的工作线程
        cv_.notify_one();
        return true;
    }

private:
    void worker_loop() {
        while (true) {
            Task task;

            {
                // TODO 5：使用 unique_lock 锁住 mtx_
                std::unique_lock<std::mutex>u_lock(mtx_);

                // TODO 6：等待“队列非空，或者线程池开始停止”
                cv_.wait(u_lock,[this]{
                    return !tasks_.empty() || stopping_;
                });
                // TODO 7：如果开始停止且队列为空，退出循环
                if(tasks_.empty() && stopping_){
                    break;
                }
                // TODO 8：移动队头任务到 task，再删除队头
                task = std::move(tasks_.front());
                tasks_.pop();
            }

            // TODO 9：在锁外执行 task
            task();
        }
    }
    std::queue<Task> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

ThreadPool::ThreadPool(std::size_t thread_count) {
    if (thread_count == 0) {
        throw std::invalid_argument("线程数量必须大于 0");
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
        // TODO 1：
        // 使用 lock_guard 锁住 mtx_，
        // 将 stopping_ 设置为 true。
        std::lock_guard<std::mutex>lg_lock(mtx_);
        stopping_ = true;
    }

    // TODO 2：
    // 唤醒所有工作线程。
    cv_.notify_all();
    for (auto& worker : workers_) {
        // TODO 3：
        // 如果 worker.joinable()，调用 worker.join()。
        if(worker.joinable()){
            worker.join();
        }
    }
}
void produce(std::queue<Task>& tasks,
             std::mutex& mtx,
             std::condition_variable& cv,
             int start) {
    for (int i = 0; i < kTasksPerProducer; ++i) {
        int id = start + i;

        {
            std::lock_guard<std::mutex> lock(mtx);
            tasks.push([id]{std::cout<< "执行任务"<< id <<std::endl;});
        }// 释放锁

        cv.notify_one(); // 再通知消费者
    }
}

void consume(std::queue<Task>& tasks,
             std::mutex& mtx,
             std::condition_variable& cv,
             bool& finished) {
    while (true) {
        Task task;

        {
            std::unique_lock<std::mutex> lock(mtx);

            cv.wait(lock, [&tasks, &finished] {
                return !tasks.empty() || finished;
            });//ps：wait() 的判断函数返回 true，表示“可以继续执行”，返回 false 才表示“需要等待”。

            if (finished && tasks.empty()) {
                break;
            }

            // TODO 1：
            // 将队头任务移动到局部变量 task，
            // 然后删除队头元素。
            task = std::move(tasks.front());
            tasks.pop();
        } // 释放队列锁

        // TODO 2：
        // 调用 task，执行任务。
        task();
        // std::cout<< "执行任务" << task() <<std::endl;
    }

    std::cout << "消费者退出\n";
}

int main() {

    {
        ThreadPool pool(2);
        for (int id = 0; id < 20; ++id) {
            pool.submit([id] {
                std::cout << "执行任务：" << id << '\n';
            });
        }
    }

    std::cout << "线程池已销毁，全部任务结束\n";
    return 0;
}