#include <cstdio>
#include <cstring>
#include <poll.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    const char* mode = argc > 1 ? argv[1] : "idle";
    int data_pipe[2];
    int stop_pipe[2];

    if (::pipe(data_pipe) == -1) {
        std::perror("pipe: data");
        return 1;
    }
    if (::pipe(stop_pipe) == -1) {
        std::perror("pipe: stop");
        ::close(data_pipe[0]);
        ::close(data_pipe[1]);
        return 1;
    }

    int write_fd = -1;
    if (std::strcmp(mode, "data") == 0) {
        write_fd = data_pipe[1];
    } else if (std::strcmp(mode, "stop") == 0) {
        write_fd = stop_pipe[1];
    }

    int status = 0;
    if (write_fd >= 0 && ::write(write_fd, "X", 1) != 1) {
        std::perror("write");
        status = 1;
    } else {
        pollfd watched[2] = {
            {stop_pipe[0], POLLIN, 0},
            {data_pipe[0], POLLIN, 0}
        };

        const int ready = ::poll(watched, 2, 1000);

        if (ready < 0) {
            std::perror("poll");
            status = 1;
        } else if (ready == 0) {
            std::puts("等了 1 秒，没有通知");
        } else if (watched[0].revents & POLLIN) {
            std::puts("收到停止通知");
        } else if (watched[1].revents & POLLIN) {
            std::puts("数据可以读取");
        }
    }

    ::close(stop_pipe[0]);
    ::close(stop_pipe[1]);
    ::close(data_pipe[0]);
    ::close(data_pipe[1]);
    return status;
}
