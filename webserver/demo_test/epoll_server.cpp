#include <cstdint>
#include <cstdio>
#include <iostream>        // C++标准输入输出
#include <arpa/inet.h>     // IP地址转换函数
#include <netinet/in.h>    // sockaddr_in、端口/地址常量定义
#include <sys/socket.h>    // socket核心API：socket()、bind()、listen()、accept()、send()、recv()
#include <unistd.h>        // Linux系统调用：close()、read()、write()
#include <cerrno>          // errno 全局错误号 + 错误宏
#include <cstring>         // memset、strcpy、strlen 字符串/内存操作
#include <thread>          // C++11 std::thread 多线程
#include <vector>
#include <mutex>
#include <queue>
#include <map>
#include <string>
#include <stdexcept>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <utility>
#include <cstdlib>

#include "fd.hpp"
#include "socket_utils.hpp"
#include "threadpool.hpp"

constexpr int kPort    = 8888;  //端口号
constexpr std::size_t kBufSize = 1024;

//切换事件属性
bool modify_event(int epfd, int fd, std::uint32_t interest) {
    epoll_event event{};
    // 设置关注的事件
    event.events = interest;
    // 设置事件返回时携带的 fd
    event.data.fd = fd;
    // 调用 epoll_ctl，失败时打印错误并返回 false
    if(::epoll_ctl(epfd,EPOLL_CTL_MOD,fd,&event) == -1){    //成功返回0,失败返回-1
        std::perror("epoll_ctl MOD");
        return false;
    }
    return true;
}

struct Connection {
    explicit Connection(FdGuard&& fd)
        : socket(std::move(fd)) {}

    FdGuard socket;
    char output_buf[kBufSize];
    std::size_t size = 0;  // 有效字节数
    std::size_t sent = 0;  // 已经发送字节数
    std::uint64_t id = 0;  // 区分不同次建立的连接
    
};
struct TaskResult {
    int fd;                        // 客户端 fd
    std::uint64_t connection_id;   // 本次连接的唯一编号
    std::string data;              // 工作线程生成的响应
};

//异常处理函数
void handle_error(const std::string& what,const int ret) {
    if(ret < 0){
        const int error = errno;
        throw std::runtime_error(what + ":" + strerror(error));
    }
}

//
void handle_echo(const int client_fd){
    try{
        std::vector<char>recv_buf(kBufSize);
        while(true){
            ssize_t n = ::recv(client_fd,recv_buf.data(),recv_buf.size(),0);
            if(n > 0){
                if(!send_all(client_fd,recv_buf.data(),static_cast<std::size_t>(n))){
                    std::perror("send_all");
                    break;
                }
            }else if(n == 0){
                std::cout<<"\n[info] 客户端关闭连接"<<std::endl;
                break;
            }else{
                if(errno == EINTR){
                    continue;
                }else{
                    std::perror("recv");
                    break;                    
                }
            }
        }
    }catch(...){
        ::shutdown(client_fd,SHUT_RDWR);
    }
}

//非阻塞：当前没有数据时，立即返回，让程序有机会处理其他连接。
void set_nonblocking(int fd){
    // 读取原文件的状态标志
    int flags = ::fcntl(fd,F_GETFL,0);
    handle_error("fcntl F_GETFL", flags);

    // 保留原来的标志，并加入非阻塞标志
    handle_error("fcntl F_SETFL",::fcntl(fd, F_SETFL, flags | O_NONBLOCK));//O_NONBLOCK为非阻塞模式
}

//向 epoll 登记可读事件
void add_read_event(int epollfd, int fd) {
    epoll_event event{};

    event.events = EPOLLIN;  // 关注可读事件，默认使用 LT
    event.data.fd = fd;      // 关联的文件描述符（通常是监听socket）

    handle_error("epoll_ctl ADD", ::epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event));//EPOLL_CTL_ADD代表新增监听
}

bool send_pending(Connection& conn) {
    if (conn.sent == conn.size) {
        return true;  // 没有剩余字节
    }
    const ssize_t n = ::send(conn.socket.get(), conn.output_buf + conn.sent, conn.size - conn.sent, 0);
    if (n > 0) {
        conn.sent += static_cast<std::size_t>(n);
        return true;
    } 
    if (n < 0){
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return true; // 发送缓冲区满，稍后再试
        }
        std::perror("send");
    }
    return false;
}


int main(int argc, char const *argv[]){
    try{
        struct sockaddr_in server_addr{};
        // 1.设置服务端地址和端口
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(kPort);
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

        // 2.创建套接字socketfd,用于监听socket
        FdGuard socketfd (::socket(AF_INET,SOCK_STREAM,0));
        handle_error("socket",socketfd.get());

        // 3.地址复用
        /*
         *  给服务端 Socket 开启地址复用，方便服务端退出后重新绑定原来的 IP 和端口。
         *  避免出现“bind: Address already in use“的情况
        */
        int opt = 1; 
        handle_error("setsockopt",::setsockopt(socketfd.get(),SOL_SOCKET,SO_REUSEADDR, &opt,sizeof(opt)));
        // 4.绑定bind
        handle_error("bind", ::bind(socketfd.get(),reinterpret_cast<const sockaddr*>(&server_addr),sizeof(server_addr)));
        // 5.监听listen
        handle_error("listen",::listen(socketfd.get(),1024));
        /*
         * 利用非阻塞 I/O 实现高性能
        */ 
        // 监听 Socket 设置为非阻塞
        set_nonblocking(socketfd.get());

        // 创建 epoll，继续使用 FdGuard 管理它
        FdGuard epollfd(::epoll_create1(0));   //epoll实例，用于管理时间监听
        handle_error("epoll_create1", epollfd.get());
        
        add_read_event(epollfd.get(), socketfd.get());  // 先登记监听 Socket
        
        std::map<int, Connection> clients_map; // 保存所有客户端的所有者，避免本轮处理结束就关闭连接
        
        epoll_event events[16]{};   // 一次最多取得 16 个就绪事件，不代表只能连接 16 个客户端

        //创建线程池，2个工作线程，最大任务队列长度64
        std::queue<TaskResult> results;
        std::mutex results_mtx;

        FdGuard result_event(::eventfd(0, EFD_NONBLOCK));
        handle_error("eventfd", result_event.get());
        add_read_event(epollfd.get(), result_event.get());
        const int notify_fd = result_event.get();// 任务借用这个 fd，关闭责任仍由 result_event 持有。

        ThreadPool pool(2, 64);
        const bool accepted = pool.submit([] {
            const int result = 2 + 3;
            std::cerr << "[worker] 2 + 3 = " << result << '\n';
        });

        if (!accepted) {
            std::cerr << "[error] 线程池启动测试任务提交失败\n";
        }

        std::uint64_t next_connection_id = 1; //定义计数器
        
        while (true) {
    /**
     * int epoll_wait(int __epfd, epoll_event *__events, int __maxevents, int __timeout)
     * 
     * int __epfd   // 使用的epoll对应套接字
     * epoll_event *__events    // 将就绪事件写入这个数组
     * int __maxevents  // 一次最多返回多少个就绪事件
     * int __timeout  // 没有事件时等待的时间，单位毫秒
     * 
     * 返回值：返回就绪事件的数量，0表示超时，-1表示出错
     */
            int count = ::epoll_wait(epollfd.get(), events, 16, -1);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                handle_error("epoll_wait", count);
            }
            // count，本次有count个就绪事件，逐个处理本次就绪的事件
            for (int i = 0; i < count; ++i) {
                int fd = events[i].data.fd;
                auto ready = events[i].events;

                if (fd == result_event.get()) {
                    eventfd_t notices = 0;

                    if (::eventfd_read(fd, &notices) == -1) {	//读取累计通知次数，并把通知计数清零
                        if (errno == EINTR || errno == EAGAIN) {
                            continue;
                        }
                        handle_error("eventfd_read", -1);
                    }

                    std::queue<TaskResult> ready_results;
                    {
                        std::lock_guard<std::mutex> lock(results_mtx);
                        ready_results.swap(results);    // 持锁交换两个队列，把当前结果整批取到局部队列里
                    }

                    while (!ready_results.empty()) {
                        TaskResult result = std::move(ready_results.front());
                        ready_results.pop();

                        auto it = clients_map.find(result.fd);

                        // 连接已关闭或 fd 被复用，就丢弃旧结果。
                        if (it == clients_map.end() || it->second.id != result.connection_id) {
                            continue;
                        }

                        Connection& target = it->second;
                        bool should_close = false;

                        // 当前使用固定数组，响应必须能放进去。
                        if (result.data.empty() || result.data.size() > sizeof(target.output_buf)) {
                            std::cerr << "响应长度异常，关闭连接\n";
                            should_close = true;
                        } else {
                            // 把工作线程的响应复制到该连接的发送缓冲区。
                            std::memcpy(target.output_buf, result.data.data(), result.data.size());
                            target.size = result.data.size();
                            target.sent = 0;

                            should_close = !modify_event(epollfd.get(), result.fd, EPOLLOUT);
                        }

                        if (should_close) {
                            // 这里清理的是结果所属的客户端。
                            if (::epoll_ctl(epollfd.get(), EPOLL_CTL_DEL, result.fd, nullptr) == -1) {
                                std::perror("epoll_ctl DEL");
                            }
                            clients_map.erase(it);
                        }
                    }

                    continue;
                }
                // 情况一：监听 Socket 就绪，有新连接可以接收
                if (fd == socketfd.get()) {
                    int raw_fd = ::accept(socketfd.get(), nullptr, nullptr); //从监听 Socket 接收新连接，拿到客户端 fd
                    if (raw_fd < 0) {
                        if (errno == EINTR ||errno == EAGAIN ||errno == EWOULDBLOCK) {
                            continue;
                        }
                        handle_error("accept", raw_fd);
                    }

                    FdGuard client_fd(raw_fd);  //接管客户端 fd，负责关闭它

                    set_nonblocking(client_fd.get());  // 新接受的客户端 Socket 设置非阻塞

                    add_read_event(epollfd.get(), client_fd.get());  // 将客户端加入 epoll

                    clients_map.emplace(raw_fd, Connection(std::move(client_fd)));   // 把关闭责任转交给连接表
                    clients_map.at(raw_fd).id = next_connection_id++;  // at() 取得刚插入的连接，分配编号后计数器加一。
                    std::cout << "[info] 新连接，fd=" << raw_fd 
                              << ", id=" << clients_map.at(raw_fd).id << '\n';
                    continue;
                }
                // 情况二：已连接的客户端发生事件
                auto it = clients_map.find(fd);
                if (it == clients_map.end()) {
                    continue;                                
                }
                Connection& conn = it->second;  // 获取连接对象
                bool should_close = false; // 事件关闭标记
                if (ready & (EPOLLERR | EPOLLHUP)) { // 检查这次事件有无错误标志
                    should_close = true;
                } else if (conn.sent < conn.size) {
                    // 有待发数据，本轮只处理发送，防止 recv 覆盖缓冲区。
                    if (ready & EPOLLOUT) {
                        if (!send_pending(conn)) {
                            should_close = true;
                        } else if (conn.sent == conn.size) {
                            should_close = !modify_event(epollfd.get(), fd, EPOLLIN);
                        }
                    }
                } else if (ready & EPOLLIN) {
                    const ssize_t n = ::recv(fd, conn.output_buf, sizeof(conn.output_buf), 0);
                    if (n > 0) {
                        conn.size = static_cast<std::size_t>(n);
                        conn.sent = 0;
                        //添加任务
                        const std::string payload(conn.output_buf, conn.size);
                        const std::uint64_t connection_id = conn.id;

                        const bool accepted = pool.submit(
                            [payload, fd, connection_id, &results, &results_mtx, notify_fd] {
                                // 三个值按 TaskResult 的成员顺序初始化。
                                TaskResult result{fd, connection_id, payload};

                                {
                                    std::lock_guard<std::mutex> lock(results_mtx);
                                    results.push(std::move(result));  // 将响应移入共享队列
                                }

                                int ret;
                                do {
                                    ret = ::eventfd_write(notify_fd, 1);  // 入队后通知事件线程
                                } while (ret == -1 && errno == EINTR);

                                if (ret == -1 && errno != EAGAIN) {
                                    std::perror("eventfd_write");
                                }
                            }
                        );
                        if (!accepted) {
                            std::cerr << "统计任务提交失败\n";
                            should_close = true;
                        }else{
                             // 暂停读写通知，等待工作线程返回响应。
                            should_close = !modify_event(epollfd.get(), fd, 0);
                        }
                    } else if (n == 0) {
                        should_close = true;
                    } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                        std::perror("recv");
                        should_close = true;
                    }
                }

                if (should_close) {
                    // 先取消登记
                    if (::epoll_ctl(epollfd.get(), EPOLL_CTL_DEL, fd, nullptr) == -1) {
                        std::perror("epoll_ctl DEL");
                    }
                    // 删除所有者，FdGuard 析构并关闭 Socket
                    clients_map.erase(fd);
                }
            }
        }
    }
    catch(const std::runtime_error& e){
        std::cerr << "异常" << e.what() <<std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
