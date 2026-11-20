cmake_minimum_required(VERSION 3.13)
project(sh3001_gyro LANGUAGES CXX)

add_executable(gyro_cpp test.cpp)

set_target_properties(gyro_cpp PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(gyro_cpp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../webserver/demo_test/include"
)

target_compile_options(gyro_cpp PRIVATE -Wall -Wextra)