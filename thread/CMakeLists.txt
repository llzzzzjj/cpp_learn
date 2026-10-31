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

#生成可执行文件
add_executable(threadpool_test
    threadpool.cpp
    threadpool_test.cpp
)

# 线程库pthread
target_link_libraries(threadpool_test PRIVATE Threads::Threads)
target_compile_options(threadpool_test PRIVATE -Wall -Wextra -Wpedantic)

# 设置编译选项（-Wall警告，-g调试，-O2优化）
add_compile_options(-Wall -g)