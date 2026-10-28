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

# 公共头文件 + 线程库：所有 target 都通过链接它获得 -Iinclude 和 -pthread
add_library(common INTERFACE)
target_include_directories(common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(common INTERFACE Threads::Threads)

# ───────────────────────── 正式程序 ─────────────────────────
add_executable(server src/server.cpp)
target_link_libraries(server PRIVATE common)

add_executable(client src/client.cpp)
target_link_libraries(client PRIVATE common)

# ───────────────────────── 学习用的 demo ─────────────────────────
# 不需要时：cmake -DBUILD_DEMOS=OFF ..
option(BUILD_DEMOS "Build the programs under demo_test/" ON)

if(BUILD_DEMOS)
    # 修正后的完整版本
    add_executable(server_demo demo_test/server_demo.cpp)
    target_link_libraries(server_demo PRIVATE common)

    # 早期的 pthread 版本（保留用于对照，带已知警告）
    add_executable(server_test demo_test/server_test.cpp)
    target_link_libraries(server_test PRIVATE common)
endif()
