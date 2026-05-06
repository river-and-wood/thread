#include "concurrency/lock_guard.hpp"
#include "concurrency/mutex.hpp"
#include "concurrency/shared_mutex.hpp"
#include "concurrency/thread_pool.hpp"

#include <future>
#include <iostream>
#include <thread>
#include <vector>

namespace {

void run_mutex_counter_demo() {
    // 保护 shared_counter 的互斥锁。
    concurrency::Mutex counter_mutex;

    // 多个线程共同递增的共享计数器。
    int shared_counter = 0;

    // thread_count 表示启动多少个工作线程。
    const int thread_count = 4;

    // increments_per_thread 表示每个线程递增计数器的次数。
    const int increments_per_thread = 10000;

    // 保存所有工作线程对象，便于后续 join。
    std::vector<std::thread> workers;

    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        workers.push_back(std::thread([&]() {
            for (int step = 0; step < increments_per_thread; ++step) {
                // guard 构造时加锁，析构时自动解锁。
                concurrency::LockGuard<concurrency::Mutex> guard(counter_mutex);
                ++shared_counter;
            }
        }));
    }

    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index].join();
    }

    std::cout << "Mutex counter result: " << shared_counter << std::endl;
}

void run_shared_mutex_demo() {
    // 保护 cached_value 的读写锁。
    concurrency::SharedMutex cache_mutex;

    // 模拟缓存中的共享数据：写线程更新，读线程读取。
    int cached_value = 0;

    // writer 是唯一写线程，使用独占写锁更新 cached_value。
    std::thread writer([&]() {
        for (int value = 1; value <= 5; ++value) {
            concurrency::UniqueLockGuard<concurrency::SharedMutex> guard(cache_mutex);
            cached_value = value;
            std::cout << "writer updates value to " << cached_value << std::endl;
        }
    });

    // readers 保存多个读线程，它们使用共享读锁读取 cached_value。
    std::vector<std::thread> readers;
    for (int reader_id = 0; reader_id < 3; ++reader_id) {
        readers.push_back(std::thread([&, reader_id]() {
            for (int round = 0; round < 5; ++round) {
                // 多个 SharedLockGuard 可以同时存在，但会和写锁互斥。
                concurrency::SharedLockGuard<concurrency::SharedMutex> guard(cache_mutex);
                std::cout << "reader " << reader_id
                          << " sees value " << cached_value << std::endl;
            }
        }));
    }

    writer.join();
    for (std::size_t index = 0; index < readers.size(); ++index) {
        readers[index].join();
    }
}

void run_thread_pool_demo() {
    // 创建一个包含 4 个工作线程的线程池。
    // 后续提交的任务会被放入队列，并由这些 worker 并发执行。
    concurrency::ThreadPool pool(4);

    // 每个任务返回 int，所以这里保存 std::future<int>。
    // future 代表“未来某个时刻会产生的结果”。
    std::vector<std::future<int> > results;

    for (int value = 1; value <= 8; ++value) {
        // enqueue() 接收一个 lambda，把它包装进任务队列。
        // 捕获 value 使用按值捕获，避免循环变量变化影响任务计算。
        results.push_back(pool.enqueue([value]() {
            return value * value;
        }));
    }

    int sum = 0;
    for (std::size_t index = 0; index < results.size(); ++index) {
        // get() 会等待对应任务完成，并取出返回值。
        // 如果任务中抛出异常，也会在这里重新抛出。
        sum += results[index].get();
    }

    std::cout << "ThreadPool square sum: " << sum << std::endl;
}

} // namespace

int main() {
    run_mutex_counter_demo();
    run_shared_mutex_demo();
    run_thread_pool_demo();
    return 0;
}
