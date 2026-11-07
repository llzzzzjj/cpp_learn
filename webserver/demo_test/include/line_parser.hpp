#pragma once  // 防止同一源文件重复包含

#include <cstddef>
#include <string>

inline bool extract_line(std::string& input_buf, std::string& line) {
    const auto pos = input_buf.find('\n');

    if (pos == std::string::npos) {
        return false;  // 数据还没凑齐一行
    }

    const std::size_t len = pos + 1;
    line = input_buf.substr(0, len);
    input_buf.erase(0, len);  // 保留后面的数据

    return true;
}