cmake_minimum_required(VERSION 3.10)

project(cpp11_concurrency_locks
        VERSION 0.1.0
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(concurrency_locks
    src/mutex.cpp
    src/shared_mutex.cpp
)

target_include_directories(concurrency_locks
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(concurrency_locks
    PRIVATE
        -Wall -Wextra -Wpedantic
)

find_package(Threads REQUIRED)
target_link_libraries(concurrency_locks PUBLIC Threads::Threads)

add_executable(lock_demo examples/lock_demo.cpp)
target_link_libraries(lock_demo PRIVATE concurrency_locks)

add_executable(lock_tests tests/lock_tests.cpp)
target_link_libraries(lock_tests PRIVATE concurrency_locks)

enable_testing()
add_test(NAME lock_tests COMMAND lock_tests)
