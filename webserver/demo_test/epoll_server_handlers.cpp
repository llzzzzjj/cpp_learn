#include "epoll_server.hpp"
#include "line_parser.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {
// 业务函数接收一条完整请求，返回带换行的响应。
std::string process_request(const std::string& input) {
    // 收到 fail 加换行时，故意抛异常，用于验证错误处理。
    if (input == "fail\n") {
        throw std::runtime_error("模拟业务处理失败");
    }
    std::string response = input;  // 保存一份可以修改的数据

    for (char& ch : response) {
        // ① 判断 ch 是否在 'a' 到 'z' 之间。
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');  // ② 如果是，转换成对应的大写字母。
        }
    }
    return response;
}
}

// 主线程接入一个新连接，为它注册可读事件并分配连接编号。
void EpollServer::on_accept_ready() {
    // 从监听 Socket 接受一个新连接，拿到客户端的 fd。
    const int raw_fd = ::accept(listen_fd_.get(), nullptr, nullptr);
    if (raw_fd < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return;  // 结束这次处理，主循环继续处理后面的事件。
        }
        handle_error("accept", raw_fd);
    }

    // 接管新 Socket，设置非阻塞，并让 epoll 关注它的可读事件。
    FdGuard client_fd(raw_fd);
    set_nonblocking(client_fd.get());
    add_read_event(client_fd.get());

    // 把所有权移入连接表，再给本次连接分配编号。
    clients_.emplace(raw_fd, Connection(std::move(client_fd)));
    Connection& conn = clients_.at(raw_fd);
    conn.id = next_connection_id_++;

    std::cout << "[info] 新连接，fd=" << raw_fd
              << ", id=" << conn.id << '\n';
}

// fd 是客户端 Socket，ready 是 epoll_wait 返回的就绪标志。
void EpollServer::on_client_ready(int fd, std::uint32_t ready) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return;  // 连接已不在表里，结束这次事件处理。
    }

    Connection& conn = it->second;
    // 统一记录当前事件是否要求关闭连接。
    bool should_close = false;

    if (ready & (EPOLLERR | EPOLLHUP)) {
        // 遇到错误或挂断，交给函数末尾统一清理。
        should_close = true;
    } else if (conn.sent < conn.size) {
        // 有待发响应时，只处理发送，避免 recv 覆盖发送缓冲区。
        if (ready & EPOLLOUT) {
            if (!send_pending(conn)) {
                should_close = true;
            } else if (conn.sent == conn.size) {
                // 响应发完后，输入缓冲区里可能已有下一条完整请求。
                conn.size = 0;
                conn.sent = 0;
                should_close = !dispatch_next(conn);
            }
        }
    } else if (!conn.worker_pending && (ready & EPOLLIN)) {
        const ssize_t n = ::recv(fd, conn.output_buf, sizeof(conn.output_buf), 0);
        if (n > 0) {
            // 只追加实际读到的字节，再尝试取出一条完整请求。
            conn.input_buf.append(conn.output_buf, static_cast<std::size_t>(n));
            should_close = !dispatch_next(conn);
        } else if (n == 0) {
            should_close = true;  // 对端已经结束发送。
        } else if (errno != EINTR && errno != EAGAIN &&
                   errno != EWOULDBLOCK) {
            std::perror("recv");
            should_close = true;
        }
    }

    if (should_close) {
        close_connection(fd);
    }
}

// 非阻塞发送一次。返回 true 表示连接可以继续，可能还有字节待发。
bool EpollServer::send_pending(Connection& conn) {
    if (conn.sent == conn.size) {
        return true;  // 没有剩余字节
    }
    // 保存本次准备发送的长度，用于判断是否只发出了一部分。
    const std::size_t remaining = conn.size - conn.sent;
    // 起始地址跳过已发送部分，长度只计算尚未发送的字节。
    const ssize_t n = ::send(
        conn.socket.get(),
        conn.output_buf + conn.sent,
        conn.size - conn.sent,
        MSG_NOSIGNAL
    ); // 对端断开时，通过返回值处理发送错误
    if (n > 0) {
        conn.sent += static_cast<std::size_t>(n);
        if (static_cast<std::size_t>(n) < remaining) {
            ++conn.short_writes;
        }
        return true;
    }
    if (n < 0) {
        if (errno == EINTR) {
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ++conn.send_would_block;
            return true;  // 保留发送进度，等下次 EPOLLOUT。
        }
        std::perror("send");
    }
    return false;
}

bool EpollServer::stage_response(Connection& conn, const std::string& response) {
    if (response.empty() || response.size() > sizeof(conn.output_buf)) {return false;}

    // 交给原有 EPOLLOUT 分支发送，保留短发送进度。
    std::memcpy(conn.output_buf, response.data(), response.size());
    conn.size = response.size();
    conn.sent = 0;
    return modify_event(conn.socket.get(), EPOLLOUT);
}

// 主线程从连接缓冲区尝试提取一行，按需恢复读关注或提交工作任务。
bool EpollServer::dispatch_next(Connection& conn) {
    // request 接收 extract_line 提取出的完整请求。
    std::string request;
    const int fd = conn.socket.get();

    // 没有完整行时，先检查是否还有空间容纳结尾的换行。
    if (!extract_line(conn.input_buf, request)) {
        if (conn.input_buf.size() >= kBufSize) {
            std::cerr << "请求过长\n";
            return false;
        }
        return modify_event(fd, EPOLLIN);
    }

    // 完整行的长度已包含换行，恰好达到上限仍然合法。
    if (request.size() > kBufSize) {
        std::cerr << "请求过长\n";
        return false;
    }

    // extract_line 保留行尾；命令和回包都复用原有分帧。
    if (request == "STATUS\n" || request == "STATUS\r\n") {return stage_response(conn, make_status_response());}
    if (request == "SUB light\n" || request == "SUB light\r\n") {
        conn.light_subscribed = true;
        return stage_response(conn, "OK SUB light\n");
    }
    if (request == "SUB gyro\n" || request == "SUB gyro\r\n") {
        conn.gyro_subscribed = true;
        return stage_response(conn, "OK SUB gyro\n");
    }
    if (request == "UNSUB\n" || request == "UNSUB\r\n") {
        conn.light_subscribed = false;
        conn.gyro_subscribed = false;
        return stage_response(conn, "OK UNSUB\n");
    }

    const std::uint64_t connection_id = conn.id;
    // 请求和连接身份由任务持有，结果队列和锁由服务器持有。
    auto& results = results_;
    auto& results_mtx = results_mtx_;
    const int notify_fd = result_event_fd.get();
    // accepted 表示任务是否入队成功，工作线程可能还没开始执行。
    const bool accepted = pool_.submit(
        [request, fd, connection_id, &results, &results_mtx, notify_fd] {
            // 工作线程稍后执行。请求和连接身份都按值捕获。
            process_and_publish(
                request, TaskResult{fd, connection_id, "ERR\n"},
                results, results_mtx, notify_fd);
        });

    if (!accepted) {
        std::cerr << "任务提交失败\n";
        return false;
    }

    // 提交成功，暂停这个连接的读写关注，等工作结果回来。
    conn.worker_pending = true;
    return modify_event(fd, 0);
}

// 工作线程执行此函数。result 已包含目标连接信息和默认错误响应。
void EpollServer::process_and_publish(const std::string& request,
                                      TaskResult result,
                                      std::queue<TaskResult>& results,
                                      std::mutex& results_mtx,
                                      int notify_fd) {
    // result 进入函数时已带有默认的 ERR 响应。
    try {
        result.data = process_request(request);
    } catch (const std::exception& error) {
        std::cerr << "[worker] 业务异常：" << error.what() << '\n';
    } catch (...) {
        std::cerr << "[worker] 未知业务异常\n";
    }

    {
        // 工作线程和主线程共用结果队列，入队时持锁。
        std::lock_guard<std::mutex> lock(results_mtx);
        results.push(std::move(result));
    }

    // 结果已经入队。解锁后通知主线程，有信号中断时重试。
    int ret;
    do {
        ret = ::eventfd_write(notify_fd, 1);
    } while (ret == -1 && errno == EINTR);

    if (ret == -1 && errno != EAGAIN) {
        std::perror("eventfd_write");
    }
}

// 主线程收取一批工作结果，为仍有效的连接开启 EPOLLOUT。
void EpollServer::on_result_ready() {
    // 读取通知计数。响应数据保存在 results 队列里。
    eventfd_t notices = 0;
    if (::eventfd_read(result_event_fd.get(), &notices) == -1) {
        if (errno == EINTR || errno == EAGAIN) {
            return;  // 结束这次通知处理。
        }
        handle_error("eventfd_read", -1);
    }

    // 持锁把结果转移到局部队列，解锁后再逐条处理。
    std::queue<TaskResult> ready_results;
    {
        std::lock_guard<std::mutex> lock(results_mtx_);
        ready_results.swap(results_);
    }

    while (!ready_results.empty()) {
        TaskResult result = std::move(ready_results.front());
        ready_results.pop();

        // fd 有可能被复用，要同时核对本次连接的编号。
        auto it = clients_.find(result.fd);
        if (it == clients_.end() || it->second.id != result.connection_id) {
            continue;  // 只跳过当前结果。
        }

        Connection& target = it->second;
        target.worker_pending = false;  // 连接编号已核对，可交付这个结果。
        bool should_close = false;

        if (result.data.empty() ||
            result.data.size() > sizeof(target.output_buf)) {
            std::cerr << "响应长度异常，关闭连接\n";
            should_close = true;
        } else {
            // 准备发送缓冲区，真正发送由 EPOLLOUT 分支负责。
            std::memcpy(target.output_buf, result.data.data(),
                        result.data.size());
            target.size = result.data.size();
            target.sent = 0;

            std::cout << "[main] 准备发送工作结果，连接编号="
                      << result.connection_id
                      << "，字节数=" << target.size << '\n';

            should_close = !modify_event(result.fd, EPOLLOUT);
        }
        if (should_close) {
            close_connection(result.fd);
        }
    }
}

// 主线程统一取消事件登记并释放客户端 Socket。
void EpollServer::close_connection(int fd) {
    // 连接关闭前打印一行汇总，方便用连接编号区分 A 和 B。
    const auto it = clients_.find(fd);
    if (it != clients_.end()) {
        const Connection& conn = it->second;
        std::cout << "[tx] id=" << conn.id
                << " skip_worker=" << conn.push_skipped_worker
                << " skip_pending=" << conn.push_skipped_pending
                << " short_writes=" << conn.short_writes
                << " would_block=" << conn.send_would_block << '\n';
    }
    // 取消这个客户端的事件登记。
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr) == -1) {
        std::perror("epoll_ctl DEL");
    }

    clients_.erase(fd);  // 删除所有者，FdGuard 自动关闭 Socket
}
