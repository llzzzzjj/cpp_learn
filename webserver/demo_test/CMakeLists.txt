# 1. 指定cmake最低版本（必须放第一行）
cmake_minimum_required(VERSION 3.16)

# 2.项目名称，可选指定语言 C / CXX（C++）
project(SocketPractice LANGUAGES CXX)

# 3. 设置C++标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON) # 强制要求该标准
set(CMAKE_CXX_EXTENSIONS OFF)       # 关闭gcc扩展，标准C++

# 4. 生成可执行文件：add_executable(目标名 源文件列表)
add_executable(server_test server_test.cpp socket_utils.cpp)
add_executable(client_test client_test.cpp socket_utils.cpp)
add_executable(echo_server echo_server.cpp socket_utils.cpp)
add_executable(epoll_server epoll_server.cpp socket_utils.cpp)

# 5.添加头文件搜索路径

# target_include_directories(server_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
# target_include_directories(client_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
# target_include_directories(echo_server PRIVATE ${PROJECT_SOURCE_DIR}/include)
# target_include_directories(epoll_server PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(server_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(client_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(echo_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(epoll_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# 查找线程库（配合 Threads::Threads 使用）
find_package(Threads REQUIRED)

# 6. 链接库（线程库pthread，写TCP多线程必备！）
target_link_libraries(server_test PRIVATE Threads::Threads)
target_compile_options(server_test PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(client_test PRIVATE Threads::Threads)
target_compile_options(client_test PRIVATE -Wall -Wextra -Wpedantic)

target_compile_options(echo_server PRIVATE -Wall -Wextra -Wpedantic)

target_compile_options(epoll_server PRIVATE -Wall -Wextra -Wpedantic)

# 设置编译选项（-Wall警告，-g调试，-O2优化）
add_compile_options(-Wall -g)