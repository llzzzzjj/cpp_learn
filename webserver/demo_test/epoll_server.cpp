#include "epoll_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {
// 服务端的默认 TCP 端口。交互客户端和协议脚本都连接这个端口。
constexpr int kPort = 8888;
// 单次 epoll_wait 最多取出的就绪事件数。
constexpr int kMaxReadyEvents = 16;
}

// 创建监听 Socket、事件通知资源和线程池，再向 epoll 登记三个入口 fd。
EpollServer::EpollServer(int port)
    : listen_fd_(create_listener(port)),
      epoll_fd_(checked_fd(::epoll_create1(0), "epoll_create1")),
      result_event_fd(checked_fd(::eventfd(0, EFD_NONBLOCK), "eventfd")),
      stop_event_fd(create_stop_event()), 
      pool_(2, 64) {
    add_read_event(listen_fd_.get());
    add_read_event(result_event_fd.get());
    add_read_event(stop_event_fd.get());
}

// 主线程的事件循环。每个就绪项按 fd 分发到对应处理函数。
void EpollServer::run() {
    epoll_event events[kMaxReadyEvents]{};
    while (true) {
        // count 是本轮收到的事件数。超时参数 -1 表示一直等待事件。
        const int count = ::epoll_wait(epoll_fd_.get(), events, kMaxReadyEvents, -1);
        if (count == -1) {
            if (errno == EINTR) {
                continue;
            }
            handle_error("epoll_wait", count);
        }

        // 每个就绪事件只交给与其 fd 对应的处理函数。
        for (int i = 0; i < count; ++i) {
            // data.fd 是登记时填入的标识，events[i].events 是本轮就绪掩码。
            const int fd = events[i].data.fd;
            if (fd == stop_event_fd.get()) {
                if (on_stop_ready()) {
                    return;
                }
            } else if (fd == result_event_fd.get()) {
                on_result_ready();
            } else if (fd == listen_fd_.get()) {
                on_accept_ready();
            } else {
                on_client_ready(fd, events[i].events);
            }
        }
    }
}

// 用于失败时返回负数并设置 errno 的接口，what 用来标记出错的操作。
void EpollServer::handle_error(const char* what, int ret) {
    if (ret < 0) {
        const int error = errno;
        throw std::runtime_error(std::string(what) + ": " + std::strerror(error));
    }
}

// 检查新建 fd，再把关闭责任交给 FdGuard。
FdGuard EpollServer::checked_fd(int fd, const char* what) {
    handle_error(what, fd);
    return FdGuard(fd);
}

// 保留现有文件状态标志，并加入 O_NONBLOCK。
void EpollServer::set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    handle_error("fcntl F_GETFL", flags);
    handle_error("fcntl F_SETFL", ::fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}

// 创建已绑定、已监听的非阻塞 IPv4 TCP Socket，并转移其所有权。
FdGuard EpollServer::create_listener(int port) {
    FdGuard listener = checked_fd(::socket(AF_INET, SOCK_STREAM, 0), "socket");

    const int reuse = 1;
    handle_error("setsockopt",
                 ::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
                              &reuse, sizeof(reuse)));

    // htons 将端口转成网络字节序，INADDR_ANY 表示监听所有本地 IPv4 地址。
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    handle_error("bind",
                 ::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address),
                        sizeof(address)));
    handle_error("listen", ::listen(listener.get(), 1024));
    set_nonblocking(listener.get());
    return listener;
}

// 屏蔽 SIGINT 并建立 signalfd，让退出信号进入同一个事件循环。
FdGuard EpollServer::create_stop_event() {
    sigset_t mask;
    handle_error("sigemptyset", ::sigemptyset(&mask));
    handle_error("sigaddset", ::sigaddset(&mask, SIGINT));

    // 在线程池构造之前屏蔽 SIGINT，工作线程会继承这个掩码。
    // pthread_sigmask 直接返回错误号，失败时用 error 获取错误说明。
    const int error = ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    if (error != 0) {
        throw std::runtime_error(std::string("pthread_sigmask: ") + std::strerror(error));
    }
    return checked_fd(::signalfd(-1, &mask, SFD_NONBLOCK), "signalfd");
}

// 登记一个 fd 的可读事件，默认使用水平触发。
void EpollServer::add_read_event(int fd) {
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = fd;
    handle_error("epoll_ctl ADD", ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &event));
}

// interest 是新的关注掩码，fd 是已登记的目标。失败时由调用方决定如何清理。
bool EpollServer::modify_event(int fd, std::uint32_t interest) {
    epoll_event event{};
    event.events = interest;
    event.data.fd = fd;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &event) == -1) {
        std::perror("epoll_ctl MOD");
        return false;
    }
    return true;
}

// 主线程读取退出信号，返回 true 时 run() 结束循环。
bool EpollServer::on_stop_ready() {
    signalfd_siginfo info{};
    if (::read(stop_event_fd.get(), &info, sizeof(info)) == -1) {
        if (errno == EINTR || errno == EAGAIN) {
            return false;
        }
        handle_error("read signalfd", -1);
    }

    if (info.ssi_signo == SIGINT) {
        std::cout << "[main] 收到退出信号，开始清理\n";
        return true;
    }
    return false;
}

// 创建服务器并运行事件循环，统一报告启动或运行失败。
int main() {
    try {
        EpollServer server(kPort);
        server.run();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "异常：" << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

