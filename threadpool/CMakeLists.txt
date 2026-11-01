#cmake最低版本要求
cmake_minimum_required(VERSION 3.16)

#项目名称
project(ThreadPractice LANGUAGES CXX)

#设置C++标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON) # 强制要求该标准
set(CMAKE_CXX_EXTENSIONS OFF)       # 关闭gcc扩展，标准C++

# 查找线程库（配合 Threads::Threads 使用）
find_package(Threads REQUIRED)

# 创建库目标
add_library(threadpool STATIC
    threadpool.cpp
)

# 提供头文件搜索路径
target_include_directories(threadpool PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(threadpool PUBLIC Threads::Threads)
# 设置编译选项（-Wall警告，-g调试，-O2优化）
target_compile_options(threadpool PRIVATE -Wall -Wextra -Wpedantic)

#生成可执行文件
add_executable(threadpool_test
    threadpool_test.cpp
)

# 链接创建的线程池库
target_link_libraries(threadpool_test PRIVATE threadpool)
target_compile_options(threadpool_test PRIVATE -Wall -Wextra -Wpedantic)

# 启用测试
enable_testing()

# 将测试程序注册到 CTest
add_test(
    NAME threadpool_regression
    COMMAND threadpool_test
)

# 整个测试程序最多运行 30 秒
set_tests_properties(threadpool_regression PROPERTIES
    TIMEOUT 30
)
