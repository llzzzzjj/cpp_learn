#include <cstdio>
#include <iostream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cerrno>
#include <vector>
#include <thread>
#include <string>
#include <cstring>
#include <stdexcept>

#include "fd.hpp"
#include "socket_utils.hpp"

constexpr int kPort = 8888;
// constexpr const char* kServerIp = "192.168.137.45";

void handle_error(const std::string& what, int ret) {
    if(ret < 0) {
        throw std::runtime_error(what + ": " + strerror(errno));
    }
}

// 接收线程借用 client_fd 并打印响应，由 main() 管理 Socket 的寿命。
void recv_from_server(int client_fd) {
    // 一次 recv 的临时缓冲区，打印时只使用实际收到的字节数。
    std::vector<char> buf(1024);
    while(true) {
        ssize_t n = ::recv(client_fd, buf.data(), buf.size(), 0);
        if(n > 0) {
            std::cout.write(buf.data(), static_cast<std::streamsize>(n));
            std::cout.flush();
        } else if(n == 0) {
            std::cout << "\n[info] 服务端关闭连接\n";
            break;
        } else {
            if(errno == EINTR) continue;
            std::cerr << "[recv error] " << strerror(errno) << std::endl;
            break;
        }
    }
}

int main() {
    try {
        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(kPort);

        std::string server_ip;
        while (true) {
            std::cout << "请输入服务端 IPv4 地址（例如 192.168.137.45）：";
            if (!std::getline(std::cin, server_ip)) {
                return EXIT_FAILURE;
            }

            const int result = ::inet_pton(
                AF_INET, server_ip.c_str(), &server_addr.sin_addr);
            handle_error("inet_pton", result);

            if (result == 1) {
                break;
            }
            std::cerr << "IPv4 地址格式无效，请重新输入。\n";
        }

        FdGuard server_fd(::socket(AF_INET, SOCK_STREAM, 0));
        handle_error("socket", server_fd.get());

        handle_error("connect", ::connect(
            server_fd.get(),
            reinterpret_cast<const sockaddr*>(&server_addr),
            sizeof(server_addr)));

        std::cout << "已连接到服务端 " << server_ip << ':' << kPort << '\n';

        // 主线程读取并发送消息，接收线程同时打印服务端响应。
        std::thread recv_th(recv_from_server, server_fd.get());

        std::string line;
        while (std::getline(std::cin, line)) {
            line += '\n';
            if (!send_all(server_fd.get(), line.data(), line.size())) {
                std::cerr << "[send error] " << strerror(errno) << std::endl;
                break;
            }
        }

        ::shutdown(server_fd.get(), SHUT_WR);
        recv_th.join();
    } catch (const std::runtime_error& e) {
        std::cerr << "异常：" << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return 0;
}
