cmake_minimum_required(VERSION 3.16)

project(tcp_blocking LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 查找线程库（配合 Threads::Threads 使用）
find_package(Threads REQUIRED)

add_executable(echo_server echo_server.cpp socket_utils.cpp)

target_include_directories(echo_server PRIVATE "${PROJECT_SOURCE_DIR}/include")

