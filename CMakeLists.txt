cmake_minimum_required(VERSION 3.16)
project(AdaptiveThreadPool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# Core library
add_library(thread_pool_lib
    src/task_queue.cpp
    src/thread_pool.cpp
    src/adaptive_controller.cpp
)
target_include_directories(thread_pool_lib PUBLIC include)
target_link_libraries(thread_pool_lib PUBLIC Threads::Threads)

# Benchmark executable
add_executable(benchmark benchmarks/benchmark_main.cpp)
target_link_libraries(benchmark PRIVATE thread_pool_lib)

# Tests executable
add_executable(tests tests/test_main.cpp)
target_link_libraries(tests PRIVATE thread_pool_lib)
