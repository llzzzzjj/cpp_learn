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
constexpr int kPort = 8888;
constexpr int kMaxReadyEvents = 16;
}

EpollServer::EpollServer(int port)
    : listen_fd_(create_listener(port)),
      epoll_fd_(checked_fd(::epoll_create1(0), "epoll_create1")),
      result_event_(checked_fd(::eventfd(0, EFD_NONBLOCK), "eventfd")),
      stop_event_(create_stop_event()),
      pool_(2, 64) {
    add_read_event(listen_fd_.get());
    add_read_event(result_event_.get());
    add_read_event(stop_event_.get());
}

void EpollServer::run() {
    epoll_event events[kMaxReadyEvents]{};
    while (true) {
        const int count = ::epoll_wait(epoll_fd_.get(), events, kMaxReadyEvents, -1);
        if (count == -1) {
            if (errno == EINTR) {
                continue;
            }
            handle_error("epoll_wait", count);
        }

        // 每个就绪事件只交给与其 fd 对应的处理函数。
        for (int i = 0; i < count; ++i) {
            const int fd = events[i].data.fd;
            if (fd == stop_event_.get()) {
                if (on_stop_ready()) {
                    return;
                }
            } else if (fd == result_event_.get()) {
                on_result_ready();
            } else if (fd == listen_fd_.get()) {
                on_accept_ready();
            } else {
                on_client_ready(fd, events[i].events);
            }
        }
    }
}

void EpollServer::handle_error(const char* what, int ret) {
    if (ret < 0) {
        const int error = errno;
        throw std::runtime_error(std::string(what) + ": " + std::strerror(error));
    }
}

FdGuard EpollServer::checked_fd(int fd, const char* what) {
    handle_error(what, fd);
    return FdGuard(fd);
}

void EpollServer::set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    handle_error("fcntl F_GETFL", flags);
    handle_error("fcntl F_SETFL", ::fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}

FdGuard EpollServer::create_listener(int port) {
    FdGuard listener = checked_fd(::socket(AF_INET, SOCK_STREAM, 0), "socket");

    const int reuse = 1;
    handle_error("setsockopt",
                 ::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
                              &reuse, sizeof(reuse)));

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

FdGuard EpollServer::create_stop_event() {
    sigset_t mask;
    handle_error("sigemptyset", ::sigemptyset(&mask));
    handle_error("sigaddset", ::sigaddset(&mask, SIGINT));

    // 在线程池构造之前屏蔽 SIGINT，工作线程会继承这个掩码。
    const int error = ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    if (error != 0) {
        throw std::runtime_error(std::string("pthread_sigmask: ") + std::strerror(error));
    }
    return checked_fd(::signalfd(-1, &mask, SFD_NONBLOCK), "signalfd");
}

void EpollServer::add_read_event(int fd) {
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = fd;
    handle_error("epoll_ctl ADD", ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &event));
}

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

bool EpollServer::on_stop_ready() {
    signalfd_siginfo info{};
    if (::read(stop_event_.get(), &info, sizeof(info)) == -1) {
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

