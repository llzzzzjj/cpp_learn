cmake_minimum_required(VERSION 3.16)

project(webserver LANGUAGES CXX)

# ───────────────────────── 编译选项 ─────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)                  # 用 -std=c++17 而不是 -std=gnu++17

# 导出 compile_commands.json，VSCode / clangd 可以直接用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 默认 Debug；命令行可覆盖：cmake -DCMAKE_BUILD_TYPE=Release ..
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# ───────────────────────── 依赖 ─────────────────────────
find_package(Threads REQUIRED)

# 公共头文件 + 线程库：所有 target 都通过链接它获得 -include 和 -pthread
add_library(common INTERFACE)
target_include_directories(common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(common INTERFACE Threads::Threads)

# ───────────────────────── 正式程序 ─────────────────────────
add_executable(server src/server.cpp)
target_link_libraries(server PRIVATE common)

add_executable(client src/client.cpp)
target_link_libraries(client PRIVATE common)
