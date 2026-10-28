/**文件描述符FD 封装
 * 
 */
#pragma once
#include <unistd.h>
#include <utility>

class FileDescriptor {
public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }

    // fd 独占所有权，禁止拷贝
    FileDescriptor(const FileDescriptor&)            = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    FileDescriptor& operator=(FileDescriptor&& o) noexcept {
        if (this != &o) {
            if (fd_ >= 0) ::close(fd_); //::表示去全局作用域找 close()函数
            fd_   = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }

    int  get()   const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int  release() noexcept { int t = fd_; fd_ = -1; return t; }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0 && fd_ != fd) ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};