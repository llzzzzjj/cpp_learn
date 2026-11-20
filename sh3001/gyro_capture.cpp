#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>
#include <sstream>

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
GyroOffsets read_gyro_offsets();
FdGuard open_gyro_input();

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

int main() {
    try {
        const GyroOffsets offsets = read_gyro_offsets();

        FdGuard control_fd(::open("/dev/gyrosensor", O_RDWR));
        if (control_fd.get() < 0) {
            throw std::system_error(
                errno, std::generic_category(), "open /dev/gyrosensor");
        }

        FdGuard input_fd = open_gyro_input();

        std::vector<GyroSample> samples;
        {
            GyroSession session(control_fd.get());
            samples = capture_samples(input_fd.get(), kSampleCount);
        }  // session 在这里析构，关闭传感器上报

        print_samples(samples, offsets);
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}