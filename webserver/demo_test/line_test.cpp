#include <iostream>
#include <string>
#include "line_parser.hpp"

int main() {
    std::string input_buf = "hel";
    std::string line;

    // 半条请求，应该提取失败，原数据仍然保留。
    if (extract_line(input_buf, line) || input_buf != "hel") {
        std::cerr << "半条请求测试失败\n";
        return 1;
    }

    input_buf += "lo\nwor";  // 现在是 hello\nwor

    // 取出完整第一行，保留后面尚未完成的 wor。
    if (!extract_line(input_buf, line) ||
        line != "hello\n" || input_buf != "wor") {
        std::cerr << "完整请求测试失败\n";
        return 1;
    }
    input_buf = "hello\nworld\n";  // 模拟两条请求一起到达

    // 第一次取出 hello\n，留下 world\n。
    if (!extract_line(input_buf, line) ||
        line != "hello\n" || input_buf != "world\n") {
        std::cerr << "提取第一条请求失败\n";
        return 1;
    }

    // 第二次取出 world\n，缓冲区应变为空。
    if (!extract_line(input_buf, line) ||
        line != "world\n" || !input_buf.empty()) {
        std::cerr << "提取第二条请求失败\n";
        return 1;
    }

    std::cout << "全部测试通过\n";
    return 0;
}