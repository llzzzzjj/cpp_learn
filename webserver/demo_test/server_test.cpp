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
void read_from_client(const int client_fd){
    try{
        std::vector<char> read_buf(kBufSize);
        while(true){
            //ssize_t n = ::recv(client_fd,&read_buf,1024,0);   //X &read_buf 是 vector 对象自身的地址，不是它管理的数据区。接收数据会破坏对象内存。
            ssize_t n = ::recv(client_fd,read_buf.data(),read_buf.size(),0);
            if(n > 0){
                // 按实际字节数输出，不依赖 '\0' 结尾
                std::cout.write(read_buf.data(),static_cast<std::streamsize>(n));
                std::cout.flush();
            }else if(n == 0){
                std::cout<<"\n[info] 客户端关闭连接"<<std::endl;
                break; // recv 返回 0 后必须退出，否则会反复空转。
            }else{
                if(errno == EINTR)  continue;
                std::perror("recv");
                break;
            }
        }
    }catch(...){
        ::shutdown(client_fd,SHUT_RDWR);
    }
    // ::close(client_fd);
}

void send_to_client(const int client_fd){
    std::string line;
    while(std::getline(std::cin,line)){
        line += "\n";
        if (!send_all(client_fd, line.data(), line.size())) {
            std::perror("send_all");
            ::shutdown(client_fd, SHUT_RDWR);
            return;
        }

    }
    // 只停止发送，仍然允许接收线程接收数据
    int ret;
    do {
        ret = ::shutdown(client_fd, SHUT_WR);
    } while(ret < 0 && errno == EINTR);
    if (ret < 0 && errno != ENOTCONN) {
        std::perror("shutdown SHUT_WR");
    }
    std::cout << "[info] 发送线程结束\n";
    // ::close(client_fd);// 不 close，main 中的 FdGuard 负责最终关闭
}

int main(int argc, char const *argv[]){
    try{
        struct sockaddr_in server_addr{};
        //1.设置服务端地址和端口
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(kPort);
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        //2.创建套接字server_fd
        FdGuard server_fd (::socket(AF_INET,SOCK_STREAM,0));
        handle_error("socket",server_fd.get());
        // 给服务端 Socket 开启地址复用，方便服务端退出后重新绑定原来的 IP 和端口。避免出现“bind: Address already in use“的情况
        int opt = 1; 
        handle_error("setsockopt",::setsockopt(server_fd.get(),SOL_SOCKET,SO_REUSEADDR, &opt,sizeof(opt)));
        // 3.绑定套接字server_fd
        handle_error("bind", ::bind(server_fd.get(),reinterpret_cast<const sockaddr*>(&server_addr),sizeof(server_addr)));
        // 4.监听套接字server_fd
        handle_error("listen",::listen(server_fd.get(),1024));
        std::cout <<"服务端启动，监听端口为："<< kPort <<std::endl;

        struct sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);
        int clientfd = ::accept(server_fd.get(),reinterpret_cast<sockaddr*>(&client_addr),&client_addr_len);
        handle_error("accept",clientfd);

        FdGuard client_fd(clientfd);

        char ip_buf[INET_ADDRSTRLEN]{0};
        inet_ntop(AF_INET,&client_addr.sin_addr,ip_buf,INET_ADDRSTRLEN);
        std::cout << "已连接客户端：" << ip_buf << ":" << ntohs(client_addr.sin_port) << " clientfd=" << clientfd << std::endl;

        std::thread read_thread(read_from_client,client_fd.get());
        std::thread write_thread(send_to_client,client_fd.get());

        //等待线程结束
        read_thread.join();
        write_thread.join(); 
    
    }
    catch(const std::runtime_error& e){
        std::cerr << "异常" << e.what() <<std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
