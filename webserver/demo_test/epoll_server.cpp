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
#include <sstream>  // C++ 标准库：在内存里拼接状态响应。
#include <stdexcept>
#include <system_error>  // C++11 标准库：保留通知失败的 errno。

#include <optional>  // C++17 标准库：表达“还没有样本”。

namespace {
// 服务端的默认 TCP 端口。交互客户端和协议脚本都连接这个端口。
constexpr int kPort = 8888;
// 单次 epoll_wait 最多取出的就绪事件数。
constexpr int kMaxReadyEvents = 16;
constexpr std::int64_t kGyroPushIntervalUs = 100000;  // 陀螺仪推送间隔：100 ms。

void post_sensor_event(int fd) {
    for (;;) {
        if (::eventfd_write(fd, 1) == 0) {return;}
        const int error = errno;
        if (error == EINTR) {continue;}
        if (error == EAGAIN) {return;}  // 计数器已可读，接收方仍能醒来。
        throw std::system_error(error, std::generic_category(), "eventfd_write sensor");
    }
}
}

// 先登记通知入口，最后启动采集线程。
EpollServer::EpollServer(int port, bool enable_sensors)
    : listen_fd_(create_listener(port)),
      epoll_fd_(checked_fd(::epoll_create1(0), "epoll_create1")),
      result_event_fd(checked_fd(::eventfd(0, EFD_NONBLOCK), "eventfd")),
      // 和线程池结果通知分开，主循环才能根据 fd 区分来源。
      sensor_event_fd_(checked_fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC), "eventfd sensor")),
      sensor_stop_fd_(checked_fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC), "eventfd sensor stop")),
      stop_event_fd(create_stop_event()), 
      pool_(2, 64) {
    add_read_event(listen_fd_.get());
    add_read_event(result_event_fd.get());
    add_read_event(sensor_event_fd_.get());
    add_read_event(stop_event_fd.get());
    if (enable_sensors) {start_sensor_capture();}
}

void EpollServer::start_sensor_capture() {
    sensor_thread_ = std::thread([this] {
        try {
            capture_dual_sensors(
                sensor_stop_fd_.get(),
                [this](const GyroSample& sample) {
                    // 先发布样本，再通知主线程来取。
                    gyro_queue_.push(sample);
                    post_sensor_event(sensor_event_fd_.get());
                },
                [this](const LightSample& sample) {
                    light_queue_.push(sample);
                    post_sensor_event(sensor_event_fd_.get());
                });
        } catch (const std::exception& error) {
            // 单独报告采集失败，TCP 仍可继续服务。
            std::fprintf(stderr, "[sensor] 采集退出：%s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "[sensor] 采集退出：未知异常\n");
        }
    });
}

EpollServer::~EpollServer() noexcept {
    if (!sensor_thread_.joinable()) {return;}

    // 采集线程借用这些成员，唤醒并等它停用设备后再销毁成员。
    try {
        post_sensor_event(sensor_stop_fd_.get());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[sensor] 停止通知失败：%s\n", error.what());
    }
    sensor_thread_.join();
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
            }  else if (fd == sensor_event_fd_.get()) {
                on_sensor_ready();
            }else if (fd == listen_fd_.get()) {
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

void EpollServer::on_sensor_ready() {
    eventfd_t count{};
    int result;

    // 读走累计通知，避免水平触发反复报告同一次可读。
    do {
        result = ::eventfd_read(sensor_event_fd_.get(), &count);
    } while (result < 0 && errno == EINTR);

    if (result < 0 && errno == EAGAIN) {return;}
    handle_error("eventfd_read sensor", result);

    // 本轮每路最多取一个队列容量，避免持续采样占住 socket 事件循环。
    // 本轮中途新增的样本仍会有单独的通知。
    GyroSample gyro{};
    for (std::size_t index = 0; index < kSensorQueueCapacity && gyro_queue_.try_pop(gyro); ++index) {
        // 保存本轮取到的最新样本，供状态查询使用。
        latest_gyro_ = gyro;
        ++gyro_consumed_;
        if (gyro_consumed_ == 1 || gyro_consumed_ % 30 == 0) {
            std::cout << "[gyro] 已消费 " << gyro_consumed_ << " 组，x=" << gyro.x
                      << " y=" << gyro.y << " z=" << gyro.z << '\n';
        }
    }

    LightSample light{};
    for (std::size_t index = 0; index < kSensorQueueCapacity && light_queue_.try_pop(light); ++index) {
        // 等级 0 也是有效样本；每次出队都更新快照和计数。
        latest_light_ = light;
        ++light_consumed_;
        publish_sensor_sample(SensorChannel::Light, "DATA light level=" + std::to_string(light.level) + '\n');
        std::cout << "[light] 等级=" << light.level << '\n';
    }

    // 光照先尝试发送；陀螺仪每轮只考虑最新样本。
    if (!latest_gyro_) {return;}
    const GyroSample& sample = *latest_gyro_;
    if (last_gyro_push_us_ && sample.elapsed_us - *last_gyro_push_us_ < kGyroPushIntervalUs) {return;}
    last_gyro_push_us_ = sample.elapsed_us;

    std::ostringstream response;
    response << "DATA gyro x=" << sample.x << " y=" << sample.y << " z=" << sample.z << '\n';
    publish_sensor_sample(SensorChannel::Gyro, response.str());
}

void EpollServer::publish_sensor_sample(SensorChannel channel, const std::string& response) {
    for (auto it = clients_.begin(); it != clients_.end();) {
        // 先推进迭代器，发送准备失败时可以安全删除当前连接。
        auto current = it++;
        const int fd = current->first;
        Connection& conn = current->second;

        // 忙碌的连接跳过本次推送，避免覆盖已有响应或插队。
        const bool subscribed = channel == SensorChannel::Gyro ? conn.gyro_subscribed : conn.light_subscribed;
        if (!subscribed) {continue;}
        if (conn.worker_pending) {
            ++conn.push_skipped_worker;
            continue;
        }
        if (conn.sent < conn.size) {
            ++conn.push_skipped_pending;
            continue;
        }
        if (!stage_response(conn, response)) {close_connection(fd);}
    }
}

std::string EpollServer::make_status_response() const {
    std::ostringstream response;
    // 快照和计数由 epoll 线程维护；三轴仍是原始值。
    response << "STATUS gyro_count=" << gyro_consumed_ << " gyro_raw=";
    if (latest_gyro_) {
        response << latest_gyro_->x << ',' << latest_gyro_->y << ',' << latest_gyro_->z;
    } else {
        response << "NA";
    }

    response << " light_count=" << light_consumed_ << " light_level=";
    if (latest_light_) {
        response << latest_light_->level;
    } else {
        response << "NA";
    }

    // 每条响应以换行结束，客户端沿用原有分帧。
    response << '\n';
    return response.str();
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
int main(int argc, char* argv[]) {
    const bool enable_sensors = argc == 2 && std::strcmp(argv[1], "--sensors") == 0;
    if (argc != 1 && !enable_sensors) {
        std::fprintf(stderr, "用法：%s [--sensors]\n", argv[0]);
        return EXIT_FAILURE;
    }

    try {
        // Ubuntu 可只跑 Echo；在 RK3568 上用 --sensors 启用真机采集。
        EpollServer server(kPort, enable_sensors);
        server.run();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "异常：" << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

