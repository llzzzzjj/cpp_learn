#pragma once

#include <cstddef>

// 阻塞 Socket：发送全部字节，或遇到错误时返回 false，并保留 errno。
// 失败前可能已发出部分数据；此时不要从头重发整条消息。
// 只借用 fd，不负责 close 或 shutdown。length 为 0 时直接成功。
bool send_all(int fd, const char* data, std::size_t length);