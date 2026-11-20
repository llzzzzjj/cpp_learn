#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <cstring>
#include <system_error>
#include <vector>
#include <sstream>
#include <fstream>
#include <stdexcept>
#include <chrono>
#include <memory>

#include <dirent.h>
#include <linux/input.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "fd.hpp"  // 复用 epoll 项目里的 FdGuard

constexpr std::size_t kSampleCount = 100;
constexpr unsigned long kSetGyroEnable = _IOW(77, 2, int);

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
FdGuard open_gyro_input() {
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
        FdGuard candidate(::openat(dirfd(directory.get()), entry->d_name, O_RDONLY | O_CLOEXEC));
        if (candidate.get() < 0) {
            continue;   // 可能权限不足，跳过
        }
        // TODO 3：用 ioctl + EVIOCGNAME 读设备名。
        //         名字是 "gyro" 就 return 候选 FdGuard，否则继续找。
        char name[256] = {};
        if (::ioctl(candidate.get(), EVIOCGNAME(sizeof(name) - 1), name) < 0) {
            continue;
        }
        if (std::strcmp(name, "gyro") == 0) {
            return candidate;
        }
    }

    throw std::runtime_error("未找到名为 gyro 的输入设备");
}

std::vector<GyroSample> capture_samples(int input_fd,
                                        std::size_t sample_count);

void print_samples(const std::vector<GyroSample>& samples,
                   const GyroOffsets& offsets);

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
        std::fprintf(stderr, "GyroSession: disable gyro failed: %s\n",
                     std::strerror(err));
    }
}

std::vector<GyroSample> capture_samples(int input_fd,
                                       std::size_t sample_count) {
    std::vector<GyroSample> samples;
    samples.reserve(sample_count);

    // TODO 1：用 steady_clock 记录开始时间，初始化三轴当前值。
    const auto start = std::chrono::steady_clock::now(); //记录开始时间
    std::int32_t gx = 0, gy = 0, gz = 0;

    // TODO 2：循环读取 input_event，直到 samples.size() == sample_count。
    //         read 被 EINTR 打断就重试；其他错误或未读到完整事件就报错。
    while(samples.size() < sample_count) {
        struct input_event ev{};
        ssize_t n;
        do{
            n = ::read(input_fd,&ev,sizeof(ev));
        }while(n < 0 && errno == EINTR);
        if (n < 0) {
            const int err = errno;
            throw std::system_error(err, std::generic_category(),
                                    "capture_samples: read input_event");
        }
        if (n != static_cast<ssize_t>(sizeof(ev))) {
            // (返回值为0) ---> 这次未读到数据；
            // (返回值 < sizeof) ---> 部分读
            throw std::runtime_error(
                "capture_samples: short read from input device");
        }
    
        // TODO 3：EV_ABS 下按 ABS_RX / ABS_RY / ABS_RZ 更新对应轴。
        if(ev.type == EV_ABS) {
            switch(ev.code){
                case ABS_RX: gx = ev.value; break;
                case ABS_RY: gy = ev.value; break;
                case ABS_RZ: gz = ev.value; break;
                default: break;
            }
        }
        // TODO 4：遇到 EV_SYN + SYN_REPORT，计算已过的微秒数，
        //         将时间和当前三轴值一起放入 samples。
        //         保存后保留三轴当前值，因为下一组不一定上报所有轴。
        if(ev.type == EV_SYN && ev.code == SYN_REPORT) {
            const auto now = std::chrono::steady_clock::now();
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
            samples.push_back(GyroSample{static_cast<std::int64_t>(us), gx, gy, gz});
        }
    }
    return samples;
}

void print_samples(const std::vector<GyroSample>& samples,
                   const GyroOffsets& offsets) {
    constexpr double kCountPerDegPerSec = 16.384; // 按照+-2000dps量程来计算  以角度/每秒单位进行计算 32768/2000 = 16.384
    // TODO：遍历 samples，按官方 demo 的量程设置，
    for(const auto& s : samples){
        const double calib_x = (static_cast<double>(s.x) - offsets.x) / kCountPerDegPerSec; 
        const double calib_y = (static_cast<double>(s.y) - offsets.y) / kCountPerDegPerSec; 
        const double calib_z = (static_cast<double>(s.z) - offsets.z) / kCountPerDegPerSec; 
        std::cout << "t = "<< s.elapsed_us <<" us "
                    "x = "<< calib_x <<"°/s "
                    "y = "<< calib_y <<"°/s "
                    "z = "<< calib_z <<"°/s \n";
    }
    //       每轴用 (原始值 - 对应轴偏移) / 16.384 换算角速度。
    //       每行输出 elapsed_us、X、Y、Z，标明单位 μs 和 °/s。
    (void)samples;
    (void)offsets;
}
int main() {
    try {
        const auto offsets = read_gyro_offsets();

        FdGuard control(::open("/dev/gyrosensor", O_RDWR | O_CLOEXEC));
        if (control.get() < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "open /dev/gyrosensor");
        }

        auto input = open_gyro_input();

        std::vector<GyroSample> samples;
        {
            GyroSession session(control.get());
            samples = capture_samples(input.get(), 3);
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