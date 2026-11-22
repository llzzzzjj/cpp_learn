#include <cerrno>
#include <chrono>
#include <cstdio>
#include <thread>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

void notify_after_delay(int stop_fd) {
    std::this_thread::sleep_for(std::chrono::seconds(1));

    int result;
    do {
        result = ::eventfd_write(stop_fd, 1);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        std::perror("eventfd_write");
    }
}

int main() {
    const int stop_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (stop_fd == -1) {
        std::perror("eventfd");
        return 1;
    }

    std::puts("开始等待停止通知");
    std::thread notifier(notify_after_delay, stop_fd);

    pollfd watched = {stop_fd, POLLIN, 0};
    const int ready = ::poll(&watched, 1, 3000);

    bool notified = false;
    if (ready < 0) {
        std::perror("poll");
    } else if (ready == 0) {
        std::puts("等待超时，未收到停止通知");
    } else if (watched.revents & POLLIN) {
        std::puts("收到停止通知，poll() 已返回");
        notified = true;
    } else {
        std::puts("poll() 返回了意外的事件");
    }

    notifier.join();
    ::close(stop_fd);
    return notified ? 0 : 1;
}
