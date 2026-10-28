#pragma once
#include <unistd.h>
#include <utility>


/**
 * **RAII 封装 fd，自动管理文件描述符生命周期，防止忘记 close、分支多导致资源泄漏
 */
class FdGuard{
public:
    explicit FdGuard(int fd):fd_(fd){};
    ~FdGuard(){
        if(fd_ >= 0){
            ::close(fd_);
        }
    }
    //禁止拷贝
    /**
     * fd 是独占资源，如果允许拷贝，两个FdGuard持有同一fd，析构时会 close 两次，UB
    */
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;

    /**
     *  允许转移所有权。std::move 之后，原来的守卫不再拥有 fd，由新对象负责关闭；
     *  适合把 fd 在函数之间传递。
    */
    //移动构造
    FdGuard(FdGuard&& other) noexcept{
        fd_ = other.fd_;
        other.fd_ = -1;
    }

    //移动赋值
    FdGuard& operator=(FdGuard&& other) noexcept{
        if(this == &other) return *this;
        //关闭当前持有的fd
        if(fd_ >= 0){
            ::close(fd_);
        }
        //接管
        fd_ = other.fd_;
        other.fd_ = -1;
        return *this;
    }

    int get()const {return fd_;}
    //释放所有权，返回fd
    int realse(){
        int temp = fd_;
        fd_ = -1;
        return temp;
    }

private:
    int fd_;
};