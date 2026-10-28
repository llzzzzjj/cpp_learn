#include <cstdio>
#include <iostream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <vector>
#include <thread>
#include <string>
#include <cstring>
#include <stdexcept>
#include "fd.hpp"
#include "socket_utils.hpp"

constexpr int kPort = 8888;
constexpr const char* kServerIp = "127.0.0.1";

void handle_error(const std::string& what, int ret) {
    if(ret < 0) {
        throw std::runtime_error(what + ": " + strerror(errno));
    }
}

void recv_from_server(int client_fd) {
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
    // ::close(client_fd);
}

int main() {
    try {
        FdGuard server_fd(::socket(AF_INET, SOCK_STREAM, 0));
        handle_error("socket", server_fd.get());

        struct sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(kPort);
        handle_error("inet_pton", inet_pton(AF_INET, kServerIp, &server_addr.sin_addr));

        handle_error("connect", ::connect(server_fd.get(), reinterpret_cast<const sockaddr*>(&server_addr), sizeof(server_addr)));
        std::cout << "已连接到服务端 " << kServerIp << ":" << kPort << std::endl;

        std::thread recv_th(recv_from_server, server_fd.get());

        std::string line;
        while(std::getline(std::cin, line)) {
            line += '\n';
            ssize_t n = ::send(server_fd.get(), line.data(), line.size(), 0);
            if(n < 0) {
                if(errno == EINTR) continue;
                std::cerr << "[send error] " << strerror(errno) << std::endl;
                break;
            }
        }
        ::shutdown(server_fd.get(), SHUT_WR);   // ← 发送 FIN：我不发了
        recv_th.join();  // 等服务端回应并关闭
    } catch(const std::runtime_error& e) {
        std::cerr << "异常：" << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return 0;
}
