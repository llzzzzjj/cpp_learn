#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <utility>

#include "fd.hpp"
#include "threadpool.hpp"

class EpollServer {
public:
    explicit EpollServer(int port);
    void run();

private:
    static constexpr std::size_t kBufSize = 1024;

    struct Connection {
        explicit Connection(FdGuard&& fd) : socket(std::move(fd)) {}

        FdGuard socket;
        char output_buf[kBufSize]{};
        std::size_t size = 0;
        std::size_t sent = 0;
        std::uint64_t id = 0;
        std::string input_buf;
    };

    struct TaskResult {
        int fd;
        std::uint64_t connection_id;
        std::string data;
    };

    static void handle_error(const char* what, int ret);
    static FdGuard checked_fd(int fd, const char* what);
    static void set_nonblocking(int fd);
    static FdGuard create_listener(int port);
    static FdGuard create_stop_event();

    void add_read_event(int fd);
    bool modify_event(int fd, std::uint32_t interest);
    void close_connection(int fd);

    bool on_stop_ready();
    void on_accept_ready();
    void on_result_ready();
    void on_client_ready(int fd, std::uint32_t ready);
    bool dispatch_next(Connection& conn);
    static bool send_pending(Connection& conn);
    static void process_and_publish(const std::string& request,
                                    TaskResult result,
                                    std::queue<TaskResult>& results,
                                    std::mutex& results_mtx,
                                    int notify_fd);

    FdGuard listen_fd_;
    FdGuard epoll_fd_;
    FdGuard result_event_;
    FdGuard stop_event_;

    std::map<int, Connection> clients_;
    std::uint64_t next_connection_id_ = 1;
    std::queue<TaskResult> results_;
    std::mutex results_mtx_;

    // 先等待工作线程退出，再销毁任务借用的队列、锁和通知 fd。
    ThreadPool pool_;
};

./bu    