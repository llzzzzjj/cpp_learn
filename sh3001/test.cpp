#include <exception>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>
#include <sstream>
#include <fstream>
#include <stdexcept>
#include <chrono>
#include <memory>
#include <functional>
#include <thread>
#include <deque>
#include <mutex>
#include <atomic>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>

// POSIX / Unix 系统头文件
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <poll.h>

// Linux 专有内核头文件
#include <linux/input.h>
#include <sys/eventfd.h>

#include "fd.hpp"  // 复用 epoll 项目里的 FdGuard

constexpr std::size_t kSampleCount = 100;
constexpr unsigned long kSetGyroEnable = _IOW(77, 2, int);
// 沿用官方 C 用例的光照启停请求码。
constexpr unsigned long kSetLightEnable = _IOW('l', 2, int*);

struct GyroOffsets {
    int x;
    int y;
    int z;
};

struct GyroSample {
    std::int64_t elapsed_us;  // 距应用开始采集过去的微秒数
    int x;
    int y;
    int z;
};

bool parse_gyro_offsets(const std::string& line, GyroOffsets& offsets){
    std::istringstream iss(line);
    // 1. 跳过 "gyro calibration"，读到 ':' 为止（':' 被丢弃）
    std::string prefix;
    std::getline(iss, prefix, ':');
    int x,y,z;
    char comma1, comma2;
    if(iss >> x >> comma1 >> y >> comma2 >> z && comma1 == ',' && comma2 == ','){
        offsets.x = x;
        offsets.y = y;
        offsets.z = z;
        return true;
    }
    return false;
}
GyroOffsets read_gyro_offsets() {
    const char* path = "/sys/class/sensor_class/gyro_calibration";
    std::ifstream ifs(path);
    if(!ifs.is_open()) {
        throw std::runtime_error(std::string("无法打开校准文件:") + path);
    }
    std::string line;
    if(!std::getline(ifs,line)) {
        throw std::runtime_error(std::string("无法读取校准值: ") + path);
    }
    GyroOffsets offsets{};
    if(!parse_gyro_offsets(line, offsets)) {
        throw std::logic_error("校准值格式错误: " + line);
    }
    return offsets;
}

//寻找陀螺仪"gyro"匹配的那个节点
FdGuard open_input_by_name(const char* device_name,int extra_flags = 0) {
    const auto close_directory = [](DIR* directory) {
        ::closedir(directory);
    };
    std::unique_ptr<DIR, decltype(close_directory)> directory(
        ::opendir("/dev/input"), close_directory);

    if (!directory) {
        throw std::system_error(errno, std::generic_category(),
                                "opendir /dev/input");
    }
    while (const auto* entry = ::readdir(directory.get())) {
        // TODO 1：用 entry->d_name 筛选 event*，比较前5个字符
        if(std::strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        // TODO 2：用 openat(dirfd(directory.get()), entry->d_name, O_RDONLY)
        //         打开候选节点，用 FdGuard 接管；打不开就继续找。
        FdGuard candidate(::openat(dirfd(directory.get()), entry->d_name, O_RDONLY | O_CLOEXEC | extra_flags));
        if (candidate.get() < 0) {
            continue;   // 可能权限不足，跳过
        }
        // TODO 3：用 ioctl + EVIOCGNAME 读设备名。
        //         名字是 "gyro" 就 return 候选 FdGuard，否则继续找。
        char name[256] = {};
        if (::ioctl(candidate.get(), EVIOCGNAME(sizeof(name) - 1), name) < 0) {
            continue;
        }
        if (std::strcmp(name, device_name) == 0) {
            return candidate;
        }
    }

    throw std::runtime_error(std::string("未找到输入设备:") + device_name);
}

FdGuard open_gyro_input(int extra_flag = 0) {
    return open_input_by_name("gyro", extra_flag);
}

std::vector<GyroSample> capture_samples(int input_fd, std::size_t sample_count);

void print_samples(const std::vector<GyroSample>& samples, const GyroOffsets& offsets);

// 借用控制节点的 fd。构造时启动上报，析构时关闭上报。
class GyroSession {
public:
    explicit GyroSession(int control_fd);
    ~GyroSession() noexcept;

    GyroSession(const GyroSession&) = delete;
    GyroSession& operator=(const GyroSession&) = delete;

private:
    int control_fd_;
};

GyroSession::GyroSession(int control_fd) : control_fd_(control_fd) {
    // TODO：准备一个值为 1 的 int，取地址传给 ioctl。
    int enable = 1;
    // TODO：调用 ioctl，把 enable 的地址传进去。
    // 请求码用 kSetGyroEnable；失败时抛 std::system_error。
    // 如果返回值 < 0，用 errno 抛出 std::system_error。
    if(::ioctl(control_fd_, kSetGyroEnable, &enable) < 0) {
        const int err = errno;
        throw std::system_error(err, std::generic_category(),"GyroSession: enable gyro");
    }
}

GyroSession::~GyroSession() noexcept {
    // TODO：准备一个值为 0 的 int，用同一个 ioctl 请求停止上报。
    //       这里不抛异常，也不 close(control_fd_)。
    const int enable = 0;
    if (::ioctl(control_fd_, kSetGyroEnable, &enable) < 0) {
        const int err = errno;
        std::fprintf(stderr, "GyroSession: disable gyro failed: %s\n", std::strerror(err));
    }
}
class LightSession {
public:
    explicit LightSession(int control_fd);
    ~LightSession() noexcept;

    LightSession(const LightSession&) = delete;
    LightSession& operator=(const LightSession&) = delete;

private:
    int control_fd_;
};
LightSession::LightSession(int control_fd) : control_fd_(control_fd) {
    // 借用控制 fd，并请求驱动开启光照上报。
    int enable = 1;
    if (::ioctl(control_fd_, kSetLightEnable, &enable) < 0) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(), "LightSession: enable light");
    }
}

LightSession::~LightSession() noexcept {
    // 请求停止上报；控制 fd 由外层的 FdGuard 关闭。
    int enable = 0;
    if (::ioctl(control_fd_, kSetLightEnable, &enable) < 0) {
        std::perror("LightSession: disable light");
    }
}

class BoundedGyroQueue{
public:
    BoundedGyroQueue(std::size_t capacity);
    void push(const GyroSample& sample);
    bool try_pop(GyroSample& sample);
    std::size_t get_dropped_count() const;

private:
    std::deque<GyroSample>gyro_que;
    std::size_t capacity_;
    std::size_t dropped_count_;
    mutable std::mutex mutex_;
};
BoundedGyroQueue::BoundedGyroQueue(std::size_t capacity)
    :capacity_(capacity), dropped_count_(0){
    if(capacity == 0) {
        throw std::invalid_argument("队列容量必须大于0!");
    }
}
void BoundedGyroQueue::push(const GyroSample& sample) {
    // 进入函数时加锁；正常返回、提前返回或抛异常时自动解锁。
    std::lock_guard<std::mutex> lock(mutex_);
    if(gyro_que.size() == capacity_) {
        gyro_que.pop_front();
        ++dropped_count_;
    }
    gyro_que.push_back(sample);
}
bool BoundedGyroQueue::try_pop(GyroSample& sample) {
    // 进入函数时加锁；正常返回、提前返回或抛异常时自动解锁。
    std::lock_guard<std::mutex> lock(mutex_);
    if(gyro_que.empty()){
        return false;
    }
    sample = gyro_que.front();
    gyro_que.pop_front();
    return true;   
}

std::size_t BoundedGyroQueue::get_dropped_count() const {
    // 进入函数时加锁；正常返回、提前返回或抛异常时自动解锁。
    std::lock_guard<std::mutex> lock(mutex_);
    // 只读取计数，不修改队列。
    return dropped_count_;
}

struct GyroAxes {
    int x = 0;
    int y = 0;
    int z = 0;
};

bool consume_gyro_event(const input_event& event, GyroAxes& axes) {

    if(event.type == EV_ABS) {
        switch(event.code){
            case ABS_RX: axes.x = event.value; break;
            case ABS_RY: axes.y = event.value; break;
            case ABS_RZ: axes.z = event.value; break;
            default: break;
        }
        return false;
    }
    return event.type == EV_SYN && event.code == SYN_REPORT;
}

std::vector<GyroSample> capture_samples(int input_fd,
                                       std::size_t sample_count) {
    std::vector<GyroSample> samples;
    samples.reserve(sample_count);

    // TODO 1：用 steady_clock 记录开始时间，初始化三轴当前值。
    const auto start = std::chrono::steady_clock::now(); //记录开始时间
    // std::int32_t gx = 0, gy = 0, gz = 0;

    // TODO 2：循环读取 input_event，直到 samples.size() == sample_count。
    //         read 被 EINTR 打断就重试；其他错误或未读到完整事件就报错。
    GyroAxes axes{};
    while(samples.size() < sample_count) {
        struct input_event ev{};
        ssize_t n;
        do{
            n = ::read(input_fd, &ev, sizeof(ev));
        }while(n < 0 && errno == EINTR);
        if (n < 0) {
            const int err = errno;
            throw std::system_error(err, std::generic_category(),"capture_samples: read input_event");
        }
        if (n != static_cast<ssize_t>(sizeof(ev))) {
            // (返回值为0) ---> 这次未读到数据；(返回值 < sizeof) ---> 部分读
            throw std::runtime_error("capture_samples: short read from input device");
        }
    
        // TODO 3：EV_ABS 下按 ABS_RX / ABS_RY / ABS_RZ 更新对应轴。
        // TODO 4：遇到 EV_SYN + SYN_REPORT，计算已过的微秒数，
        //         将时间和当前三轴值一起放入 samples。
        //         保存后保留三轴当前值，因为下一组不一定上报所有轴。
        if(consume_gyro_event(ev, axes)) {
            const auto now = std::chrono::steady_clock::now();
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
            samples.push_back(GyroSample{static_cast<std::int64_t>(us), axes.x, axes.y, axes.z});
        }
    }
    return samples;
}

void print_samples(const std::vector<GyroSample>& samples,
                   const GyroOffsets& offsets) {
    constexpr double kCountPerDegPerSec = 16.384; // 按照+-2000dps量程来计算  以角度/每秒单位进行计算 32768/2000 = 16.384
    // TODO：遍历 samples，按官方 demo 的量程设置，
    //       每轴用 (原始值 - 对应轴偏移) / 16.384 换算角速度。
    //       每行输出 elapsed_us、X、Y、Z，标明单位 μs 和 °/s。
    for(const auto& s : samples){
        const double calib_x = (static_cast<double>(s.x) - offsets.x) / kCountPerDegPerSec; 
        const double calib_y = (static_cast<double>(s.y) - offsets.y) / kCountPerDegPerSec; 
        const double calib_z = (static_cast<double>(s.z) - offsets.z) / kCountPerDegPerSec; 
        std::cout << "t = "<< s.elapsed_us <<" us "
                    "x = "<< calib_x <<"°/s "
                    "y = "<< calib_y <<"°/s "
                    "z = "<< calib_z <<"°/s \n";
    }
}
struct LightSample {
    std::int64_t elapsed_us;  // 本次双路采集开始后经过的微秒数。
    int level;                // 驱动上报的光照等级，0 也是有效值。
};

// 每收到一个有效光照事件，就调用一次这个函数。
using LightSampleHandler = std::function<void(const LightSample&)>;

using GyroSampleHandler = std::function<void(const GyroSample&)>;

void capture_continuously(int input_fd, int stop_fd, const GyroSampleHandler& on_sample) {
    GyroAxes axes{};
    const auto start = std::chrono::steady_clock::now();

    pollfd watched[] = {
        {stop_fd,  POLLIN, 0},
        {input_fd, POLLIN, 0},
    };

    for (;;) {
        int ready;
        do {
            ready = ::poll(watched, 2, -1);
        } while (ready < 0 && errno == EINTR);

        if (ready < 0) {
            const int error = errno;
            throw std::system_error(error, std::generic_category(),
                                    "capture_continuously: poll");
        }

        if (watched[0].revents & POLLIN) {
            return;
        }

        const short error_flags = POLLERR | POLLHUP | POLLNVAL;
        if ((watched[0].revents | watched[1].revents) & error_flags) {
            throw std::runtime_error("capture_continuously: fd unavailable");
        }

        if (!(watched[1].revents & POLLIN)) {
            continue;
        }

        input_event event{};
        // TODO 1：从 input_fd 读一条完整事件。EINTR 重试；F
        //         EAGAIN 回到 poll；其余错误或未读到完整事件就报错。
        ssize_t bytes_read;
        do {
            bytes_read = ::read(input_fd, &event, sizeof(event));
        } while (bytes_read < 0 && errno == EINTR);

        if (bytes_read < 0) {
            const int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK) {
                continue;
            }
            throw std::system_error(error, std::generic_category(),
                                    "capture_continuously: read");
        }

        if (bytes_read != static_cast<ssize_t>(sizeof(event))) {
            throw std::runtime_error("未读到完整的 input_event");
        }
        // TODO 2：调用 consume_gyro_event(event, axes)。
        //         返回 false，继续等待下一条事件。
        if(!consume_gyro_event(event, axes)) {
            continue;
        }
        // TODO 3：返回 true 时，像 capture_samples() 一样计算 elapsed_us，
        //         构造 GyroSample，再调用 on_sample(sample)。
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
        const GyroSample sample{
            static_cast<std::int64_t>(elapsed_us),
            axes.x,
            axes.y,
            axes.z
        };
        on_sample(sample);
    }
}

void dual_channel(int gyro_fd, int light_fd, int stop_fd, 
                  const GyroSampleHandler& on_gyro_sample, const LightSampleHandler& on_light_sample) {
    pollfd watched[] = {
        {stop_fd, POLLIN, 0},
        {gyro_fd, POLLIN, 0},
        {light_fd, POLLIN, 0}    
    };
    // 放在循环外，记录整个测试已经打印了多少条事件。
    GyroAxes axes{};
    const auto start = std::chrono::steady_clock::now();
    for(;;) {
        int ready;
        do {
            ready = ::poll(watched, 3, -1);
        } while(ready == -1 && errno == EINTR);
        if (ready < 0) {
            const int error = errno;
            throw std::system_error(error, std::generic_category(), "dual_channel: poll");
        }
        if (watched[0].revents & POLLIN) {
            return;
        }

        const short error_flags = POLLERR | POLLHUP | POLLNVAL;
        if ((watched[0].revents | watched[1].revents | watched[2].revents) & error_flags) {
            throw std::runtime_error("dual_channel: fd unavailable");
        }
        if(watched[1].revents & POLLIN) {
            input_event gyro_event{};
            ssize_t bytes_read;
            do{
                bytes_read = ::read(gyro_fd, &gyro_event, sizeof(gyro_event));
            } while(bytes_read < 0 && errno == EINTR);

            if (bytes_read < 0) {
                const int error = errno;
                if (error == EAGAIN || error == EWOULDBLOCK) {
                    continue;
                }
                throw std::system_error(error, std::generic_category(), "dual_channel: read");
            } else if (bytes_read != static_cast<ssize_t>(sizeof(gyro_event))) {
                throw std::runtime_error("未读到完整的陀螺仪输入事件");
            } else if(consume_gyro_event(gyro_event, axes)){
                // 收到 SYN_REPORT 后，把当前三轴值组装成一个样本。
                const auto elapsed_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start).count();

                const GyroSample sample{
                    static_cast<std::int64_t>(elapsed_us),
                    axes.x,
                    axes.y,
                    axes.z
                };

                // 每一组都交给调用者；是否打印由调用者决定。
                on_gyro_sample(sample);
            }
        }
        if(watched[2].revents & POLLIN) {
            input_event light_event{};
            ssize_t bytes_read;
            do {
                bytes_read = ::read(light_fd, &light_event, sizeof(light_event));
            } while(bytes_read < 0 && errno == EINTR);
            if (bytes_read < 0) {
                const int error = errno;
            // 暂时没数据时自然结束本分支，其他读取错误才报错。
            if (error != EAGAIN && error != EWOULDBLOCK) {
                throw std::system_error(error, std::generic_category(), "dual_channel: read light");
            }
            } else if (bytes_read != static_cast<ssize_t>(sizeof(light_event))) {
                throw std::runtime_error("未读到完整的光照输入事件");
            } else if (light_event.type == EV_ABS && light_event.code == ABS_MISC){
                // 两路样本共用同一个开始时间，便于比较应用收到它们的先后。
                const auto elapsed_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start).count();

                const LightSample sample{
                    static_cast<std::int64_t>(elapsed_us),
                    light_event.value
                };

                on_light_sample(sample);
            }
        }
    }
}

int run_idle_stop_test() {
    int pipe_fds[2];
    if (::pipe(pipe_fds) < 0) {
        std::perror("pipe");
        return 1;
    }

    FdGuard input(pipe_fds[0]);
    // FdGuard unused_writer(pipe_fds[1]);  // 保持写端打开，但不写数据。
    {
        FdGuard writer(pipe_fds[1]);  // 离开这个作用域，就关闭管道写端。
    }

    FdGuard stop(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (stop.get() < 0) {
        std::perror("eventfd");
        return 1;
    }

    const int stop_fd = stop.get();
    const auto start = std::chrono::steady_clock::now();

    // 通知线程等一秒，再唤醒正在 poll() 的采集函数。
    std::thread notifier([stop_fd] {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        int result;
        do {
            result = ::eventfd_write(stop_fd, 1);
        } while (result < 0 && errno == EINTR);

        if (result < 0) {
            std::perror("eventfd_write");
        }
    });

    std::size_t sample_count = 0;
    int status = 1;

    try {
        // 空管道不产生数据，因此这个回调一次也不该执行。
        capture_continuously(input.get(), stop_fd,
                             [&sample_count](const GyroSample&) {
                                 ++sample_count;
                             });

        // 在 join() 前计时，避免把通知线程的等待算进去。
        const auto elapsed_ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

        std::cout << "采集函数已返回，耗时 " << elapsed_ms
                  << " ms，收到样本 " << sample_count << " 组\n";
        status = (elapsed_ms >= 1000 && sample_count == 0) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }

    notifier.join();  // 等通知线程用完 stop_fd，再让 FdGuard 关闭它。
    return status;
}

int run_live_capture_test() {
    // 控制 fd 用来启停上报，输入 fd 用来读陀螺仪事件。
    FdGuard control(::open("/dev/gyrosensor", O_RDWR | O_CLOEXEC));
    if (control.get() < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "open /dev/gyrosensor");
    }
    auto input = open_gyro_input(O_NONBLOCK);

    FdGuard stop(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (stop.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }

    const int stop_fd = stop.get();
    std::size_t sample_count = 0;

    // 5 秒后让 poll() 醒来，走正常退出路径。
    std::thread notifier([stop_fd] {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        int result;
        do {
            result = ::eventfd_write(stop_fd, 1);
        } while (result < 0 && errno == EINTR);

        if (result < 0) {
            std::perror("eventfd_write");
        }
    });

    try {
        GyroSession session(control.get());  // 开启上报，离开作用域时停用。

        capture_continuously(input.get(), stop_fd,
                             [&sample_count](const GyroSample& sample) {
            // 每收到一组就计数，每 30 组显示一次当前原始值。
            ++sample_count;
            if (sample_count % 30 == 0) {
                std::cout << "已采集 " << sample_count << " 组"
                          << "，原始值 x=" << sample.x
                          << " y=" << sample.y
                          << " z=" << sample.z << std::endl;
            }
        });
    } catch (...) {
        notifier.join();  // 采集失败也要等通知线程结束，再把异常传给 main()。
        throw;
    }

    notifier.join();  // 通知线程结束后，stop fd 才能由 FdGuard 关闭。
    std::cout << "采集结束，共 " << sample_count << " 组\n";
    return sample_count > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int read_light_level_once(int input_fd) {
    pollfd watched{input_fd, POLLIN, 0};

    for (;;) {
        // 等输入 fd 可读；一次等待最多 5 秒。
        const int ready = ::poll(&watched, 1, 5000);
        if (ready < 0) {
            const int error = errno;
            if (error == EINTR) {
                continue;
            }
            throw std::system_error(error, std::generic_category(), "poll light");
        }
        if (ready == 0) {
            throw std::runtime_error("等待光照事件超时");
        }
        if (watched.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            throw std::runtime_error("光照输入 fd 不可用");
        }
        if (!(watched.revents & POLLIN)) {
            continue;
        }

        // 一次读一条输入事件；暂时读不到就回去等。
        input_event event{};
        const ssize_t bytes_read = ::read(input_fd, &event, sizeof(event));
        if (bytes_read < 0) {
            const int error = errno;
            if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK) {
                continue;
            }
            throw std::system_error(error, std::generic_category(), "read light");
        }
        if (bytes_read != static_cast<ssize_t>(sizeof(event))) {
            throw std::runtime_error("未读到完整的光照输入事件");
        }

        // 官方用例在 ABS_MISC 事件里直接取得光照等级。
        if (event.type == EV_ABS && event.code == ABS_MISC) {
            return event.value;
        }
    }
}

int run_dual_gyro_test() {
    // 控制 fd 用来启停上报，输入 fd 用来读陀螺仪事件。
    FdGuard control(::open("/dev/gyrosensor", O_RDWR | O_CLOEXEC));
    if (control.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "open /dev/gyrosensor");
    }
    auto gyro_input = open_gyro_input(O_NONBLOCK);

    // 用空管道模拟暂时没有事件的光照设备。
    int light_pipe[2];
    if (::pipe(light_pipe) < 0) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(), "pipe light");
    }
    FdGuard light_input(light_pipe[0]);
    FdGuard light_writer(light_pipe[1]);  // 保持写端打开，避免出现 POLLHUP。

    FdGuard stop(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (stop.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }

    const int stop_fd = stop.get();

    // 5 秒后让 poll() 醒来，走正常退出路径。
    std::thread notifier([stop_fd] {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        int result;
        do {
            result = ::eventfd_write(stop_fd, 1);
        } while (result < 0 && errno == EINTR);

        if (result < 0) {
            std::perror("eventfd_write");
        }
    });

    try {
        GyroSession session(control.get());  // 开启上报，离开作用域时停用。

        // 测试入口负责计数，每 30 组显示一次。
        std::size_t gyro_sample_count = 0;
        dual_channel(gyro_input.get(), light_input.get(), stop_fd,
                    [&gyro_sample_count](const GyroSample& sample) {
                        ++gyro_sample_count;
                        if (gyro_sample_count % 30 == 0) {
                            std::cout << "[gyro] 已采集 " << gyro_sample_count << " 组"
                                    << "，x=" << sample.x
                                    << " y=" << sample.y
                                    << " z=" << sample.z << '\n';
                        }
                    },             
                    [](const LightSample&) {// 空管道没有光照数据；收到光照样本说明测试行为不符合预期。
                        throw std::logic_error("空管道测试不应收到光照样本");
                    });
    } catch (...) {
        notifier.join();  // 采集失败也要等通知线程结束，再把异常传给 main()。
        throw;
    }

    notifier.join();  // 通知线程结束后，stop fd 才能由 FdGuard 关闭。
    // 走到这里说明采集循环正常返回，通知线程也已结束。
    std::cout << "双路读取测试结束\n";
    return EXIT_SUCCESS;
}

int run_dual_live_test() {
    // 控制 fd 用来启停上报，输入 fd 用来读陀螺仪事件。
    FdGuard gyro_control(::open("/dev/gyrosensor", O_RDWR | O_CLOEXEC));
    if (gyro_control.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "open /dev/gyrosensor");
    }
    auto gyro_input = open_gyro_input(O_NONBLOCK);

    // 控制 fd 用来启停光照上报。
    FdGuard light_control(::open("/dev/lightsensor", O_WRONLY | O_CLOEXEC));
    if (light_control.get() < 0) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(),
                                "open /dev/lightsensor");
    }

    // 输入 fd 用来读取事件，交给 dual_channel() 监听。
    auto light_input = open_input_by_name("lightsensor-level", O_NONBLOCK);
    

    FdGuard stop(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (stop.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }

    const int stop_fd = stop.get();

    // 5 秒后让 poll() 醒来，走正常退出路径。
    std::thread notifier([stop_fd] {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        int result;
        do {
            result = ::eventfd_write(stop_fd, 1);
        } while (result < 0 && errno == EINTR);

        if (result < 0) {
            std::perror("eventfd_write");
        }
    });

    try {
        GyroSession gyro_session(gyro_control.get());  // 开启上报，离开作用域时停用。
        LightSession light_session(light_control.get());

        // 测试入口负责计数，每 30 组显示一次。
        std::size_t gyro_sample_count = 0;
        dual_channel(gyro_input.get(), light_input.get(), stop_fd,
                    [&gyro_sample_count](const GyroSample& sample) {
                        ++gyro_sample_count;
                        if (gyro_sample_count % 30 == 0) {
                            std::cout << "[gyro] 已采集 " << gyro_sample_count << " 组"
                                    << "，x=" << sample.x
                                    << " y=" << sample.y
                                    << " z=" << sample.z << '\n';
                        }
                    },
                    [](const LightSample& sample) { // 真机测试每收到一条光照样本，就打印它的等级。
                        std::cout << "[light] 光照等级: " << sample.level << '\n';
                    });
    } catch (...) {
        notifier.join();  // 采集失败也要等通知线程结束，再把异常传给 main()。
        throw;
    }

    notifier.join();  // 通知线程结束后，stop fd 才能由 FdGuard 关闭。
    // 走到这里说明采集循环正常返回，通知线程也已结束。
    std::cout << "双路读取测试结束\n";
    return EXIT_SUCCESS;
}

int run_gyro_queue_live_test() {
    // 控制 fd 用来启停上报，输入 fd 用来读陀螺仪事件。
    FdGuard control(::open("/dev/gyrosensor", O_RDWR | O_CLOEXEC));
    if (control.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "open /dev/gyrosensor");
    }
    auto gyro_input = open_gyro_input(O_NONBLOCK);

    // 用空管道模拟暂时没有事件的光照设备。
    int light_pipe[2];
    if (::pipe(light_pipe) < 0) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(), "pipe light");
    }
    FdGuard light_input(light_pipe[0]);
    FdGuard light_writer(light_pipe[1]);  // 保持写端打开，避免出现 POLLHUP。

    FdGuard stop(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (stop.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }

    const int stop_fd = stop.get();
    // 用小容量故意触发溢出，并独立统计回调收到的总组数。
    constexpr std::size_t kQueueCapacity = 3;
    BoundedGyroQueue gyro_queue(kQueueCapacity);
    std::size_t received = 0;
    // 5 秒后让 poll() 醒来，走正常退出路径。
    std::thread notifier([stop_fd] {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        int result;
        do {
            result = ::eventfd_write(stop_fd, 1);
        } while (result < 0 && errno == EINTR);

        if (result < 0) {
            std::perror("eventfd_write");
        }
    });

    try {
        GyroSession session(control.get());  // 开启上报，离开作用域时停用。
        dual_channel(gyro_input.get(), light_input.get(), stop_fd,
             [&gyro_queue, &received](const GyroSample& sample) {
                 // 每组都入队，不受原来“每 30 组打印一次”的限制。
                 gyro_queue.push(sample);
                 ++received;
             },
             [](const LightSample&) {
                 throw std::logic_error("空管道测试不应收到光照样本");
             });
        } catch (...) {
            notifier.join();  // 采集失败也要等通知线程结束，再把异常传给 main()。
            throw;
        }

    notifier.join();  // 通知线程结束后，stop fd 才能由 FdGuard 关闭。
    // 采集期间只入队，停止后再取出队列里保留的样本。
    std::size_t remaining = 0;
    GyroSample sample{};
    while (gyro_queue.try_pop(sample)) {
        ++remaining;
        std::cout << "保留样本：t=" << sample.elapsed_us << " us"
                << "，x=" << sample.x << '\n';
    }

    const std::size_t dropped = gyro_queue.get_dropped_count();
    std::cout << "收到 " << received << " 组，保留 " << remaining
            << " 组，丢弃 " << dropped << " 组\n";

    // 先确认真的触发过溢出，再计算应该丢弃多少组。
    if (received <= kQueueCapacity) {
        std::cerr << "测试失败：样本太少，没有触发队列溢出\n";
        return EXIT_FAILURE;
    }
    if (remaining != kQueueCapacity || dropped != received - kQueueCapacity) {
        std::cerr << "测试失败：保留数或丢弃数不符合预期\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int run_gyro_queue_thread_test() {
    constexpr int kTotalSamples = 1000;
    BoundedGyroQueue gyro_queue(8);
    std::atomic<bool> producer_done{false};
    std::exception_ptr producer_error;

    // 生产线程用 X 值编号，方便检查出队顺序。
    std::thread producer([&gyro_queue, &producer_done, &producer_error] {
        try {
            for (int sequence = 1; sequence <= kTotalSamples; ++sequence) {
                gyro_queue.push(GyroSample{0, sequence, 0, 0});

                // 分批生产，给消费线程机会在生产期间取数据。
                if (sequence % 100 == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        } catch (...) {
            // 把异常留给主线程处理，避免异常逃出线程入口。
            producer_error = std::current_exception();
        }

        producer_done.store(true);
    });

    // 主线程充当消费者，检查取出的编号是否始终递增。
    std::size_t consumed = 0;
    int last_sequence = 0;
    bool order_ok = true;
    GyroSample sample{};

    try {
        for (;;) {
            const bool done = producer_done.load();

            if (gyro_queue.try_pop(sample)) {
                if (sample.x <= last_sequence) {
                    order_ok = false;
                }
                last_sequence = sample.x;
                ++consumed;
            } else if (done) {
                // 已经确认不会再入队，并且这次确实没取到数据。
                break;
            } else {
                // 只是暂时为空；测试里短等一下，避免空转。
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    } catch (...) {
        producer.join();
        throw;
    }

    producer.join();
    if (producer_error) {
        std::rethrow_exception(producer_error);
    }

    const std::size_t dropped = gyro_queue.get_dropped_count();
    std::cout << "入队 " << kTotalSamples << " 组，取出 " << consumed
              << " 组，丢弃 " << dropped << " 组\n";

    if (!order_ok || consumed == 0 ||
        consumed + dropped != static_cast<std::size_t>(kTotalSamples)) {
        std::cerr << "双线程队列测试失败：出队顺序或计数不符合预期\n";
        return EXIT_FAILURE;
    }

    std::cout << "双线程队列测试通过\n";
    return EXIT_SUCCESS;
}

int run_gyro_queue_eventfd_test() {
    constexpr int kTotalSamples = 1000;
    BoundedGyroQueue gyro_queue(8);
    std::atomic<bool> producer_done{false};
    std::exception_ptr producer_error;
    // 该fd用于通知消费者：该检查队列了。
    FdGuard data_ready(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (data_ready.get() < 0) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(), "eventfd data_ready");
    }
    const int ready_fd = data_ready.get();
    // 两处通知共用同一段错误处理。
    const auto notify = [ready_fd] {
        for (;;) {
            if (::eventfd_write(ready_fd, 1) == 0) {
                return;
            }
            const int error = errno;
            if (error == EINTR) {continue;}
            if (error == EAGAIN) {return;}// 计数器已经无法再加 1，已有待处理通知。
            throw std::system_error(error, std::generic_category(), "eventfd_write data_ready");
        }
    };
    // 生产线程用 X 值编号，方便检查出队顺序。
    std::thread producer([&gyro_queue, &producer_done, &producer_error, notify](int total_samples) {
        try {
            for (int sequence = 1; sequence <= total_samples; ++sequence) {
                // 先放数据，再叫醒消费者。
                gyro_queue.push(GyroSample{0, sequence, 0, 0});
                notify();
            }
        } catch (...) {
            // 把异常留给主线程处理，避免异常逃出线程入口。
            producer_error = std::current_exception();
        }

        producer_done.store(true);
        try {
            notify();
        } catch (...) {
            if (!producer_error) {
                producer_error = std::current_exception();
            }
        }
    },
    kTotalSamples);

    // 主线程充当消费者，检查取出的编号是否始终递增。
    std::size_t consumed = 0;
    int last_sequence = 0;
    bool order_ok = true;
    GyroSample sample{};

    try {
        pollfd watched{ready_fd, POLLIN, 0};
        for (;;) {
            // TODO 3：用 poll() 等 ready_fd，超时设为 2000 ms。
            //         区分 EINTR、超时和 fd 异常；只有 POLLIN 才继续读通知。
            const int ready = poll(&watched, 1, 2000);
            if(ready < 0){
                const int error = errno;
                if(error == EINTR){
                    continue;
                }
                throw std::system_error(error, std::generic_category(), "poll data_ready");
            }
            if (ready == 0) {throw std::runtime_error("等待队列通知超时");}
            if (watched.revents & (POLLERR | POLLHUP | POLLNVAL)) {throw std::runtime_error("数据通知 fd 不可用");}
            if (!(watched.revents & POLLIN)) {continue;}
        
            // TODO 4：用 eventfd_read() 读走通知，再去取样本。
            //         通知值不等于队列长度，不要拿它当出队次数。
            eventfd_t notification_count{};
            if (::eventfd_read(ready_fd, &notification_count) < 0) {
                const int error = errno;
                if (error == EINTR || error == EAGAIN) {
                    continue;
                }
                throw std::system_error(error, std::generic_category(),"eventfd_read data_ready");
            }

            // 先确认生产是否结束，再尝试取空队列。
            const bool done = producer_done.load();
            while (gyro_queue.try_pop(sample)) {
                if (sample.x <= last_sequence) {
                    order_ok = false;
                }
                last_sequence = sample.x;
                ++consumed;
            }

            if (done) {
                break;
            }
        }
    } catch (...) {
        // 等生产线程结束，再读取它保存的异常。
        producer.join();
        if (producer_error) {std::rethrow_exception(producer_error);}
        throw;
    }

    producer.join();
    if (producer_error) {std::rethrow_exception(producer_error);}

    const std::size_t dropped = gyro_queue.get_dropped_count();
    std::cout << "入队 " << kTotalSamples << " 组，取出 " << consumed
              << " 组，丢弃 " << dropped << " 组\n";

    // 允许丢旧样本，所以编号可以跳号，但不能倒退或重复。
    if (!order_ok || consumed == 0 ||
        consumed + dropped != static_cast<std::size_t>(kTotalSamples)) {
        std::cerr << "队列通知测试失败：出队顺序或计数不符合预期\n";
        return EXIT_FAILURE;
    }

    std::cout << "队列 eventfd 通知测试通过\n";
    return EXIT_SUCCESS;
}

int run_gyro_queue_live_eventfd_test() {
    BoundedGyroQueue gyro_queue(8);
    std::atomic<bool> producer_done{false};
    std::exception_ptr producer_error;
    // 该fd用于通知消费者：该检查队列了。
    FdGuard data_ready(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (data_ready.get() < 0) {
        const int error = errno;
        throw std::system_error(error, std::generic_category(), "eventfd data_ready");
    }
    const int ready_fd = data_ready.get();
    // 两处通知共用同一段错误处理。
    const auto notify = [ready_fd] {
        for (;;) {
            if (::eventfd_write(ready_fd, 1) == 0) {
                return;
            }
            const int error = errno;
            if (error == EINTR) {continue;}
            if (error == EAGAIN) {return;}// 计数器已经无法再加 1，已有待处理通知。
            throw std::system_error(error, std::generic_category(), "eventfd_write data_ready");
        }
    };
    // 控制 fd 用来启停上报，输入 fd 用来读陀螺仪事件。
    FdGuard control(::open("/dev/gyrosensor", O_RDWR | O_CLOEXEC));
    if (control.get() < 0) {throw std::system_error(errno, std::generic_category(),"open /dev/gyrosensor");}
    const int control_fd = control.get();

    FdGuard input = open_gyro_input(O_NONBLOCK);
    const int input_fd = input.get();

    FdGuard stop(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (stop.get() < 0) {throw std::system_error(errno, std::generic_category(), "eventfd");}
    const int stop_fd = stop.get();
    // 定时到了或消费端报错时，都用它请求停止采集。
    const auto request_stop = [stop_fd] {
        for (;;) {
            if (::eventfd_write(stop_fd, 1) == 0) {return;}

            const int error = errno;
            if (error == EINTR) {continue;}
            if (error == EAGAIN) {return;}  // 已有待处理的停止通知。

            std::perror("eventfd_write stop");
            return;
        }
    };
    // 只由采集线程更新；主线程等 join() 后再读取。
    std::size_t received = 0;
    std::thread producer([&gyro_queue, &received, &producer_done, &producer_error,
                        control_fd, input_fd, stop_fd, notify] {
        try {
            // 进入时启用上报，退出这个作用域时停用。
            GyroSession session(control_fd);

            capture_continuously(input_fd, stop_fd,
                                [&gyro_queue, &received, notify](const GyroSample& sample) {
                                    // TODO：入队成功后，更新收到的组数，再发送数据通知。
                                    gyro_queue.push(sample);
                                    ++received;
                                    notify();
                                });
        } catch (...) {
            producer_error = std::current_exception();
        }

        // 采集已结束；叫醒消费者，把队列里剩余的样本取完。
        producer_done.store(true);
        try {
            notify();
        } catch (...) {
            if (!producer_error) {producer_error = std::current_exception();}
        }
    });

    // 主线程充当消费者，检查取出的编号是否始终递增。
    std::size_t consumed = 0;
    GyroSample sample{};

    std::thread notifier;
    try {
        // 到时间只发停止通知，由采集线程自己退出并停用设备。
        notifier = std::thread([request_stop] {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            request_stop();
        });
        pollfd watched{ready_fd, POLLIN, 0};
        for (;;) {
            // TODO 3：用 poll() 等 ready_fd，超时设为 7000 ms。
            //         区分 EINTR、超时和 fd 异常；只有 POLLIN 才继续读通知。
            const int ready = poll(&watched, 1, 7000);
            if(ready < 0){
                const int error = errno;
                if(error == EINTR){
                    continue;
                }
                throw std::system_error(error, std::generic_category(), "poll data_ready");
            }
            if (ready == 0) {throw std::runtime_error("等待队列通知超时");}
            if (watched.revents & (POLLERR | POLLHUP | POLLNVAL)) {throw std::runtime_error("数据通知 fd 不可用");}
            if (!(watched.revents & POLLIN)) {continue;}
        
            // TODO 4：用 eventfd_read() 读走通知，再去取样本。
            //         通知值不等于队列长度，不要拿它当出队次数。
            eventfd_t notification_count{};
            if (::eventfd_read(ready_fd, &notification_count) < 0) {
                const int error = errno;
                if (error == EINTR || error == EAGAIN) {
                    continue;
                }
                throw std::system_error(error, std::generic_category(),"eventfd_read data_ready");
            }

            // 先确认生产是否结束，再尝试取空队列。
            const bool done = producer_done.load();
            while (gyro_queue.try_pop(sample)) {
                ++consumed;
                // 每 30 组显示一次，确认主线程在采集期间就取到了数据。
                if (consumed % 30 == 0) {
                    std::cout << "已采集 " << consumed << " 组，x=" << sample.x << '\n';
                }
            }

            if (done) {
                break;
            }
        }
    } catch (...) {
        request_stop();
        // 等生产线程结束，再读取它保存的异常。
        producer.join();
        if (notifier.joinable()) {notifier.join();}
        if (producer_error) {std::rethrow_exception(producer_error);}
        throw;
    }

    // 两个线程都收尾后，再读取采集计数或重新抛出异常。
    producer.join();
    notifier.join();
    if (producer_error) {std::rethrow_exception(producer_error);}

    const std::size_t dropped = gyro_queue.get_dropped_count();
    std::cout << "收到 " << received << " 组，消费 " << consumed
            << " 组，队列丢弃 " << dropped << " 组\n";

    // 真机采到多少由实际运行决定，退出时按实测总数对账。
    if (consumed == 0 || received != consumed + dropped) {
        std::cerr << "真机队列测试失败：没有消费到样本，或收支对不上\n";
        return EXIT_FAILURE;
    }

    std::cout << "真机队列 eventfd 通知测试通过\n";
    return EXIT_SUCCESS;
}

int main(int argc,char* argv[]) {
    //test code
    try {
        // Ubuntu 本地测试只使用空管道，不打开板上的设备节点。
        if (argc == 2 && std::strcmp(argv[1], "--test-idle-stop") == 0) {
            return run_idle_stop_test();
        }

        if (argc == 2 && std::strcmp(argv[1], "--test-gyro-que") == 0) {
            BoundedGyroQueue gyro_queue(3);
            for(int x_val = 1; x_val < 5; ++x_val){
                gyro_queue.push(GyroSample{0,x_val,0,0});
            }
            GyroSample sample{};
             for (int expected_x = 2; expected_x <= 4; ++expected_x) {
                if (!gyro_queue.try_pop(sample)) {
                    std::cerr << "测试失败：队列提前空了\n";
                    return EXIT_FAILURE;
                }
                if (sample.x != expected_x) {
                    std::cerr << "测试失败：预期 x=" << expected_x
                            << "，实际 x=" << sample.x << '\n';
                    return EXIT_FAILURE;
                }
                std::cout << "取出 x=" << sample.x << '\n';
            }
            // 三组取完后应为空，且只有入队时丢弃的那一组计入统计。
            if (gyro_queue.try_pop(sample)) {
                std::cerr << "测试失败：队列应该已经为空\n";
                return EXIT_FAILURE;
            }
            if (gyro_queue.get_dropped_count() != 1) {
                std::cerr << "测试失败：丢弃数应为 1，实际为 "
                        << gyro_queue.get_dropped_count() << '\n';
                return EXIT_FAILURE;
            }

            std::cout << "有界队列测试通过：丢弃 1 组，第四次出队返回 false\n";
            return EXIT_SUCCESS;
        }
        // 只运行 Ubuntu 本地的双线程队列测试。
        if (argc == 2 && std::strcmp(argv[1], "--test-gyro-queue-thread") == 0) {
            return run_gyro_queue_thread_test();
        }
        // 只运行 Ubuntu 本地的队列通知测试。
        if (argc == 2 && std::strcmp(argv[1], "--test-gyro-queue-eventfd") == 0) {
            return run_gyro_queue_eventfd_test();
        }
        // 单独运行真实陀螺仪入队测试。
        if (argc == 2 && std::strcmp(argv[1], "--test-gyro-queue-live") == 0) {
            return run_gyro_queue_live_test();
        }
        if (argc == 2 && std::strcmp(argv[1], "--test-gyro-queue-live-eventfd") == 0) {
            return run_gyro_queue_live_eventfd_test();
        }
        // 只验证能找到并打开光照输入节点，退出时自动关闭 fd。
        if (argc == 2 && std::strcmp(argv[1], "--test-light-input") == 0) {
            auto light_input = open_input_by_name("lightsensor-typo");
            std::cout << "已打开光照输入设备，fd=" << light_input.get() << '\n';
            return EXIT_SUCCESS;
        }
        if (argc == 2 && std::strcmp(argv[1], "--test-dual-gyro") == 0) {
            return run_dual_gyro_test();
        }
        // 只测光照：打开两个 fd，启用上报，读一次，退出时停用。
        if (argc == 2 && std::strcmp(argv[1], "--test-light-once") == 0) {
            FdGuard control(::open("/dev/lightsensor", O_WRONLY | O_CLOEXEC));
            if (control.get() < 0) {
                throw std::system_error(errno, std::generic_category(), "open /dev/lightsensor");
            }

            auto input = open_input_by_name("lightsensor-level", O_NONBLOCK);
            LightSession session(control.get());
            const int level = read_light_level_once(input.get());
            std::cout << "光照等级: " << level << '\n';
            return EXIT_SUCCESS;
        }
        // 单独运行两路真实传感器的测试。
        if (argc == 2 && std::strcmp(argv[1], "--test-dual-live") == 0) {
            return run_dual_live_test();
        }
        // 只在板上运行持续采集测试；不传参数仍走原来的 100 组模式。
        if (argc == 2 && std::strcmp(argv[1], "--test-live") == 0) {
            return run_live_capture_test();
        }
        // 有参数却没匹配任何已知模式，直接报错，不进入默认采集。
        if (argc > 1) {
            std::cerr << "参数无效：" << argv[1] << '\n';
            return EXIT_FAILURE;
        }
        const auto offsets = read_gyro_offsets();

        FdGuard control(::open("/dev/gyrosensor", O_RDWR | O_CLOEXEC));
        if (control.get() < 0) {
            throw std::system_error(errno, std::generic_category(), "open /dev/gyrosensor");
        }

        auto input = open_gyro_input();

        std::vector<GyroSample> samples;
        {
            GyroSession session(control.get());
            samples = capture_samples(input.get(), 100);
        }

        std::cout << "offsets: " << offsets.x << ' '
                  << offsets.y << ' ' << offsets.z << '\n';

        print_samples(samples, offsets);

        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}