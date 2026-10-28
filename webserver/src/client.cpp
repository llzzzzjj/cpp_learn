#include "fd.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

constexpr int         kPort    = 8888;
constexpr std::size_t kBufSize = 1024;

[[noreturn]] void die(const char* what) {
    std::perror(what);
    std::exit(EXIT_FAILURE);
}

// 读线程：只借用 fd 做 IO，不负责 close
void read_from_server(int fd) {
    std::vector<char> buf(kBufSize);
    while (true) {
        ssize_t n = ::recv(fd, buf.data(), buf.size() - 1, 0);
        if (n > 0) {
            buf[static_cast<std::size_t>(n)] = '\0';
            std::fputs(buf.data(), stdout);
            std::fflush(stdout);
        } else if (n == 0) {
            break;                              // 对端关闭
        } else {
            if (errno == EINTR) continue;
            std::perror("recv");
            break;
        }
    }
    std::printf("收到服务端的终止信号......\n");
}

// 写线程：只借用 fd 做 IO，不负责 close
void write_to_server(int fd) {
    std::string line;
    while (std::getline(std::cin, line)) {
        line += '\n';
        ssize_t sent = ::send(fd, line.data(), line.size(), 0);
        if (sent < 0) {
            if (errno == EINTR) continue;
            std::perror("send");
            break;
        }
    }
    std::printf("接收到命令行的终止信号，不再写入，关闭连接......\n");
    ::shutdown(fd, SHUT_WR);                    // 告诉服务端"我不发了"
}

int main() {
    // 唯一所有者：main 持有 FileDescriptor，析构时 close
    FileDescriptor sockfd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!sockfd.valid()) die("socket");

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(kPort);

    if (::connect(sockfd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        die("connect");

    std::printf("已连接 127.0.0.1:%d，输入内容回车发送，Ctrl-D 退出\n", kPort);

    // 关键：两个线程只拿到 int 副本，所有权仍在 main
    // std::thread 按值传递 int，每个线程内部存一份独立副本，无竞态
    std::thread reader(read_from_server, sockfd.get());
    std::thread writer(write_to_server, sockfd.get());

    reader.join();
    writer.join();

    std::printf("关闭资源\n");
    // sockfd 析构自动 close(fd)，这里什么都不用写
    return 0;
}