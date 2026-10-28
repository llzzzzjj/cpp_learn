#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <functional>
#include <utility>

constexpr int kTasksPerProducer = 10;

using Task = std::function<void()>;

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
    bool finished = false; //false->后续还可能有新任务;  true->所有生产者已经结束，不会再放入新任务
    std::queue<Task> tasks;
    std::mutex mtx;
    std::condition_variable cv;

    std::thread consumer_a(
        consume,
        std::ref(tasks),
        std::ref(mtx),
        std::ref(cv),
        std::ref(finished)
    );
    std::thread consumer_b(
        consume,
        std::ref(tasks),
        std::ref(mtx),
        std::ref(cv),
        std::ref(finished)
    );


    std::thread producer_a(
        produce,
        std::ref(tasks),
        std::ref(mtx),
        std::ref(cv),
        0
    );

    std::thread producer_b(
        produce,
        std::ref(tasks),
        std::ref(mtx),
        std::ref(cv),
        10
    );
    // 先等待所有生产者结束，确保后面不会再放入任务
    producer_a.join();
    producer_b.join();

    // 持锁修改共享的结束标记
    {
        std::lock_guard<std::mutex>lg_lock(mtx);
        finished = true;
    }   
    // 唤醒消费者，让它重新检查条件
    cv.notify_all();

    consumer_a.join();
    consumer_b.join();

    std::cout << "全部结束，剩余任务：" << tasks.size() << '\n';
    return 0;
}