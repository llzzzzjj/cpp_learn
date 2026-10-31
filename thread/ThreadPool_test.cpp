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
    void shutdown();

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

void ThreadPool::shutdown() {
    {
        // TODO 1：
        // 持有 mtx_，设置 stopping_ = true。
        std::lock_guard<std::mutex>lock(mtx_);
        stopping_ = true;
    }

    // TODO 2：
    // 通知所有等待的工作线程。
    cv_.notify_all();
    // TODO 3：
    // 遍历 workers_，
    // 对 joinable 的线程调用 join()。
    for(auto &w : workers_){
        if(w.joinable())   
            w.join();
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

bool test_queue_full(){
    //测试用参数
    bool started = false; //任务 a 已经开始运行
    bool allow_finish = false; //主线程是否允许任务 a 继续完成
    std::mutex gate_mtx; //保护上述两个标记
    std::condition_variable gate_cv; //等待标记变化

    std::atomic<int> completed{0};
    bool accepted_as_expected = false;

    {
        ThreadPool pool(1, 2);
        // 任务 a：占住唯一的工作线程，直到主线程放行
        const bool a = pool.submit([&] {
            {
                std::unique_lock<std::mutex> lock(gate_mtx);
                started = true;
                gate_cv.notify_all();
                gate_cv.wait(lock, [&] {
                    return allow_finish;
                });
            }
            ++completed;
        });

        // 当前线程池是新建且队列为空，a 应当被接受
        if (!a) {
            std::cerr << "测试失败：任务 a 未被接受\n";
            return 1;
        }

        // 等到 a 确实开始运行，避免凭时间猜测
        {
            std::unique_lock<std::mutex> lock(gate_mtx);
            gate_cv.wait(lock, [&] {
                return started;
            });
        }

        // 此时 a 正在等待放行，队列为空
        const bool b = pool.submit([&completed] {
            ++completed;
        });

        const bool c = pool.submit([&completed] {
            ++completed;
        });

        // 此时队列中有 b、c，容量已经用满
        const bool d = pool.submit([&completed] {
            ++completed;
        });

        std::cout << "提交结果（1=接受，0=拒绝）："
                  << a << ' ' << b << ' '
                  << c << ' ' << d << '\n';

        accepted_as_expected = a && b && c && !d;

        // 无论提交结果是否符合预期，都先放行 A
        {
            std::lock_guard<std::mutex> lock(gate_mtx);
            allow_finish = true;
        }
        gate_cv.notify_all();

    } // 析构等待 A、B、C 处理完毕

    std::cout << "完成任务数："
              << completed.load() << '\n';

    const bool passed =
        accepted_as_expected && completed.load() == 3;

    std::cout << (passed ? "测试通过\n" : "测试失败\n");

    return passed;
};
bool test_shutdown(){
    std::atomic<int> completed{0};
    ThreadPool pool(2);

    bool all_accepted = true;

    for (int i = 0; i < 5; ++i) {
        if (!pool.submit([&completed] {
            ++completed;
        })) {
            all_accepted = false;
        }
    }

    // 停止接收任务，并等待之前的任务完成
    pool.shutdown(); 

    std::cout << "停止后完成数："
              << completed.load() << '\n';

    // pool 仍存在，但应该拒绝新任务
    const bool accepted = pool.submit([&completed] {
        ++completed;
    });

    std::cout << "停止后提交结果："
              << accepted << '\n';

    // 验证连续调用不会重复 join
    pool.shutdown();
    std::cout << "重复停止正常返回\n";

    const bool passed =
        all_accepted &&
        !accepted &&
        completed.load() == 5;

    std::cout << (passed ? "测试通过\n" : "测试失败\n");

    return passed;
};
int main() {
    const bool queue_ok = test_queue_full();
    const bool shutdown_ok = test_shutdown();

    std::cout << "队列满测试："
              << (queue_ok ? "通过" : "失败") << '\n';

    std::cout << "主动停止测试："
              << (shutdown_ok ? "通过" : "失败") << '\n';

    return (queue_ok && shutdown_ok) ? 0 : 1;
}