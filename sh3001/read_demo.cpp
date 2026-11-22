#include <cstdio>
#include <cstring>
#include <cerrno>

#include <unistd.h>
#include <fcntl.h>

int main(int argc, char* argv[]) {
    const bool nonblocking =
        argc == 2 && std::strcmp(argv[1], "nonblock") == 0;

    int pipe_fds[2];
    if (::pipe(pipe_fds) == -1) {
        std::perror("pipe");
        return 1;
    }

    const int read_fd = pipe_fds[0];
    const int write_fd = pipe_fds[1];

    if (nonblocking) {
        const int flags = ::fcntl(read_fd, F_GETFL);
        if (flags == -1 ||
            ::fcntl(read_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            std::perror("fcntl");
            ::close(read_fd);
            ::close(write_fd);
            return 1;
        }
    }

    std::puts("准备调用 read()");
    std::fflush(stdout);

    char byte;
    const ssize_t result = ::read(read_fd, &byte, sizeof(byte));
    const int read_error = errno;

    if (result == -1 && read_error == EAGAIN) {
        std::puts("read() 立即返回：现在没有数据（EAGAIN）");
    } else {
        std::printf("read() 返回 %ld\n", static_cast<long>(result));
    }

    ::close(read_fd);
    ::close(write_fd);
    return 0;
}