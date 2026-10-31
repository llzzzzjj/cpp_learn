#include "threadpool.hpp"

#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <stdexcept>

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
            return false;
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
bool test_empty_task() {
    ThreadPool pool(2);

    Task empty_task;

    const bool accepted = pool.submit(empty_task);

    // 没有有效任务，也应当能够正常停止
    pool.shutdown();

    const bool passed = !accepted;

    std::cout << "空任务测试："
              << (passed ? "通过" : "失败") << '\n';

    return passed;
}

bool test_exception_recovery() {
    std::atomic<int> completed{0};
    ThreadPool pool(1);

    bool all_accepted = pool.submit([] {
        throw std::runtime_error("模拟任务失败");
    });

    for (int i = 0; i < 5; ++i) {
        if (!pool.submit([&completed] {
            ++completed;
        })) {
            all_accepted = false;
        }
    }

    pool.shutdown();

    const bool passed =
        all_accepted && completed.load() == 5;

    std::cout << "异常恢复测试："
              << (passed ? "通过" : "失败") << '\n';

    return passed;
}

bool test_multiple_producers() {
    constexpr int tasks_per_producer = 1000;
    constexpr int total_attempts = tasks_per_producer * 2;

    std::atomic<int> accepted{0};
    std::atomic<int> rejected{0};
    std::atomic<int> completed{0};

    ThreadPool pool(2, 8);

    // 定义生产者要执行的工作，此处尚未启动线程
    auto produce = [&] {
        for (int i = 0; i < tasks_per_producer; ++i) {
            const bool ok = pool.submit([&completed] {
                ++completed;
            });

            if (ok) {
                ++accepted;
            } else {
                ++rejected;
            }
        }
    };

    std::thread producer_a(produce);
    std::thread producer_b(produce);

    // 先等所有提交者结束，再停止线程池
    producer_a.join();
    producer_b.join();

    pool.shutdown();

    const int accepted_count = accepted.load();
    const int rejected_count = rejected.load();
    const int completed_count = completed.load();

    std::cout << "接收数：" << accepted_count
              << "，拒绝数：" << rejected_count
              << "，完成数：" << completed_count << '\n';

    const bool passed =
        accepted_count > 0 &&
        accepted_count + rejected_count == total_attempts &&
        completed_count == accepted_count;

    std::cout << "多生产者测试："
              << (passed ? "通过" : "失败") << '\n';

    return passed;
}

int main() {
    const bool queue_ok = test_queue_full();
    const bool shutdown_ok = test_shutdown();
    const bool empty_ok = test_empty_task();
    const bool exception_ok = test_exception_recovery();
    const bool producers_ok = test_multiple_producers();

    const bool all_passed =
        queue_ok &&
        shutdown_ok &&
        empty_ok &&
        exception_ok &&
        producers_ok;

    std::cout << "全部测试："
              << (all_passed ? "通过" : "失败") << '\n';

    return all_passed ? 0 : 1;
}