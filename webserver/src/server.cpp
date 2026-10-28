#include "fd.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

constexpr int         kPort    = 8888;  //端口号
constexpr std::size_t kBufSize = 1024;  //缓冲区大小

[[noreturn]] void die(const char* what) {
    std::perror(what);
    std::exit(EXIT_FAILURE);
}

// 注意：参数按值接收 FileDescriptor，由 std::thread 移动构造进来
void handle_client(FileDescriptor client_fd) {
    const int fd = client_fd.get();

    char read_buf[kBufSize];
    while (true) {
        std::memset(read_buf, 0, sizeof(read_buf));
        ssize_t n = ::read(fd, read_buf, sizeof(read_buf) - 1);
        if (n > 0) {
            std::printf("服务端接收客户端 %d 数据：%s\n", fd, read_buf);
            const char reply[] = "received~\n";
            if (::send(fd, reply, sizeof(reply) - 1, 0) < 0) std::perror("send");
        } else if (n == 0) {
            std::printf("客户端 client_fd: %d 正常关闭连接......\n", fd);
            break;
        } else {
            if (errno == EINTR) continue;     // 被信号打断，重试
            std::perror("read");
            break;
        }
    }

    const char bye[] = "receive your shutdown signal\n";
    ::send(fd, bye, sizeof(bye) - 1, 0);
    std::printf("释放 client_fd: %d 资源\n", fd);

    // 关键：这里什么都不用写，client_fd 析构时自动 close
}

int main() {
    FileDescriptor listen_fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!listen_fd.valid()) die("socket");

    int opt = 1;
    ::setsockopt(listen_fd.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(kPort);

    if (::bind(listen_fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        die("bind");
    if (::listen(listen_fd.get(), 10) < 0)
        die("listen");

    std::printf("listening on 0.0.0.0:%d\n", kPort);

    while (true) {
        sockaddr_in client_addr{};
        socklen_t   len = sizeof(client_addr);
        int cfd = ::accept(listen_fd.get(), reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            std::perror("accept");
            continue;                       // accept 失败不能退出主循环
        }

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        std::printf("与客户端 from %s at PORT %d 文件描述符：%d 建立连接\n",
                    ip, ntohs(client_addr.sin_port), cfd);

        // 所有权转移：main 不再持有 client_fd，线程负责关
        std::thread t(handle_client, FileDescriptor(cfd));
        t.detach();
        std::printf("创建子线程并处理为 detached 状态\n");
    }
}