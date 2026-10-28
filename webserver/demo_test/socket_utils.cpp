#include "socket_utils.hpp"

#include <sys/socket.h>
#include <cerrno>

bool send_all(int fd, const char* data, std::size_t length) {
    std::size_t sent = 0;

    while (sent < length) {
        const auto n = ::send(fd, data + sent, length - sent,MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            // 非空发送却返回 0，避免无限循环和使用过期的 errno。
            if (n == 0) {
                errno = EIO;
            }
            return false;
        }
    }
    return true;
}
