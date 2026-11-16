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

// 主线程管理连接和 epoll，工作线程负责执行业务并发布结果。
class EpollServer {
public:
    explicit EpollServer(int port);
    void run();

private:
    // 单条请求的上限（含换行），也是每条连接固定收发缓冲区的大小。
    static constexpr std::size_t kBufSize = 1024;

    // 单条 TCP 连接的状态，仅由事件线程访问。
    struct Connection {
        explicit Connection(FdGuard&& fd) : socket(std::move(fd)) {}

        FdGuard socket; // 持有客户端 fd。从连接表删除本对象时自动关闭。
        
        char output_buf[kBufSize]{}; // 等待发送时保存响应，空闲接收时也用作 recv 的临时缓冲区。
       
        std::size_t size = 0; // 当前响应的有效字节数。
        
        std::size_t sent = 0; // 当前响应已交给 send 的字节数，下一次从此偏移继续。
       
        std::uint64_t id = 0; // 本次连接的编号，用于区分同一个 fd 的不同使用者。
        
        std::string input_buf; // 接收累积->已经收到、尚未取走的字节(可能包含半行或多行)
    };

    // 工作线程交还给主线程的响应，fd 和连接编号必须同时匹配。
    struct TaskResult {  
        int fd; // 任务提交时的客户端 Socket fd。
       
        std::uint64_t connection_id; // 任务提交时的连接编号。
       
        std::string data; // 响应字节，包含协议行末尾的换行。
    };

    static void handle_error(const char* what, int ret);
    static FdGuard checked_fd(int fd, const char* what);
    static void set_nonblocking(int fd);
    static FdGuard create_listener(int port);
    static FdGuard create_stop_event();

    void add_read_event(int fd);
    bool modify_event(int fd, std::uint32_t interest);
    void close_connection(int fd);

    bool on_stop_ready();    //stop_event_ 就绪，判断要不要退出
    void on_accept_ready();  // 监听 fd 就绪，接入新客户端。
    void on_result_ready();  // 结果通知 fd 就绪，取出工作线程的响应。
    void on_client_ready(int fd, std::uint32_t ready);//其他 fd 就绪，处理客户端收发
   
    bool dispatch_next(Connection& conn); // 主线程尝试取出一行并提交任务。返回 false 时调用方关闭连接。
   
    static bool send_pending(Connection& conn); // 尝试发送一次并保存进度，是否发完由sent == size判断。
    // 工作线程执行业务，发布响应，再通过 notify_fd 通知主线程。
    static void process_and_publish(const std::string& request,
                                    TaskResult result,
                                    std::queue<TaskResult>& results,
                                    std::mutex& results_mtx,
                                    int notify_fd);

    FdGuard listen_fd_;         // 监听socket，有新连接时，传给 accept()
    FdGuard epoll_fd_;          // epoll实例，传给 epoll_ctl() 管理关注项，传给 epoll_wait() 取就绪事件
    FdGuard result_event_fd;      // eventfd	工作线程写通知，主线程读通知后取结果
    FdGuard stop_event_fd;        // signalfd	主线程读取 SIGINT，决定是否退出

    // 用客户端 fd 索引连接，由主线程独占访问。
    std::map<int, Connection> clients_;
    // 每接受一个新连接就分配一个新的编号。
    std::uint64_t next_connection_id_ = 1;
    // 工作线程写入、主线程取出的共享结果队列。
    std::queue<TaskResult> results_;
    // 保护 results_ 的入队与交换。
    std::mutex results_mtx_;

    // 先等待工作线程退出，再销毁任务借用的队列、锁和通知 fd。
    ThreadPool pool_;
};   