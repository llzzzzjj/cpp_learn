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
#include <string>
#include <stdexcept>

#include <sys/epoll.h>
#include <fcntl.h>
#include <map>
#include <utility>
#include <cstdlib>

#include "fd.hpp"
#include "socket_utils.hpp"

constexpr int kPort    = 8888;  //端口号
constexpr std::size_t kBufSize = 1024;

void handle_error(const std::string& what,const int ret) {
    if(ret < 0){
        const int error = errno;
        throw std::runtime_error(what + ":" + strerror(error));
    }
}
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
void add_read_event(int epfd, int fd) {
    epoll_event event{};

    event.events = EPOLLIN;  // 关注可读事件，默认使用 LT
    event.data.fd = fd;      // 事件返回时，用它识别对应的 Socket

    handle_error("epoll_ctl ADD", ::epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event));//EPOLL_CTL_ADD代表新增监听
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
        // 实现非阻塞
        // 监听 Socket 设置为非阻塞
        set_nonblocking(socketfd.get());

        // 创建 epoll，继续使用 FdGuard 管理它
        FdGuard epfd(::epoll_create1(0));   //epoll实例，用于管理时间监听
        handle_error("epoll_create1", epfd.get());

        // 先登记监听 Socket
        add_read_event(epfd.get(), socketfd.get());

        // 保存所有客户端的所有者，避免本轮处理结束就关闭连接
        std::map<int, FdGuard> clients;

        // 一次最多取得 16 个就绪事件，不代表只能连接 16 个客户端
        epoll_event events[16]{};

        while (true) {
            int count = ::epoll_wait(epfd.get(), // 使用哪个 epoll
                                     events, // 将就绪事件写入这个数组
                                     16,  // 一次最多返回多少个事件
                                     -1  // 没有事件时一直等待
                                    );
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                handle_error("epoll_wait", count);
            }
            // count，本次有count个就绪事件。
            // 逐个处理本次就绪的事件
            for (int i = 0; i < count; ++i) {
                int fd = events[i].data.fd;
                auto ready = events[i].events;

                // 情况一：监听 Socket 就绪，有新连接可以接收
                if (fd == socketfd.get()) {
                    int raw_fd = ::accept(socketfd.get(), nullptr, nullptr);
                    if (raw_fd < 0) {
                        if (errno == EINTR ||
                            errno == EAGAIN ||
                            errno == EWOULDBLOCK) {
                            continue;
                        }

                        handle_error("accept", raw_fd);
                    }

                    FdGuard client_fd(raw_fd);

                    // 新接受的客户端 Socket 也要单独设置非阻塞
                    set_nonblocking(client_fd.get());

                    // 将客户端加入 epoll
                    add_read_event(epfd.get(), client_fd.get());

                    // 把关闭责任转交给连接表
                    clients.emplace(raw_fd, std::move(client_fd));

                    std::cout << "[info] 新连接，fd="
                            << raw_fd << std::endl;
                    continue;
                }
                // 情况二：已连接的客户端发生事件
                if (clients.find(fd) == clients.end()) {
                    continue;
                }
                bool should_close = false;
                if (ready & EPOLLERR) {
                    should_close = true;
                } else if (ready & (EPOLLIN | EPOLLHUP)) {
                    char buf[1024];

                    // 当前就绪事件只读一次，然后继续处理其他连接
                    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);

                    if (n > 0) {
                        std::cout << "[fd=" << fd << "] 收到 "<< n << " 字节：";
                        std::cout.write(buf, static_cast<std::streamsize>(n));
                        std::cout << std::endl;
                    } else if (n == 0) {
                        std::cout << "[info] 客户端结束发送，fd="
                                << fd << std::endl;

                        should_close = true;
                    } else {
                        if (errno == EINTR ||
                            errno == EAGAIN ||
                            errno == EWOULDBLOCK) {
                            continue;
                        }
                        std::perror("recv");
                        should_close = true;
                    }
                }

                if (should_close) {
                    // 先取消登记
                    if (::epoll_ctl(epfd.get(), EPOLL_CTL_DEL, fd, nullptr) == -1) {
                        std::perror("epoll_ctl DEL");
                    }
                    // 删除所有者，FdGuard 析构并关闭 Socket
                    clients.erase(fd);
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
