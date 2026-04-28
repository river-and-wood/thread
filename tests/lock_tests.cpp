#include "concurrency/lock_guard.hpp"
#include "concurrency/mutex.hpp"
#include "concurrency/shared_mutex.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <vector>
#include <thread>

namespace {

void test_mutex_protects_counter() {
    // counter_mutex 保护 shared_counter，防止多个线程同时写入导致丢失更新。
    concurrency::Mutex counter_mutex;

    // 被多个线程共同递增的共享变量。
    int shared_counter = 0;

    // 测试线程数量。
    const int thread_count = 8;

    // 每个线程执行的递增次数。
    const int increments_per_thread = 20000;

    // 保存工作线程，确保测试结束前全部 join。
    std::vector<std::thread> workers;

    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        workers.push_back(std::thread([&]() {
            for (int step = 0; step < increments_per_thread; ++step) {
                // guard 的生命周期就是临界区范围。
                concurrency::LockGuard<concurrency::Mutex> guard(counter_mutex);
                ++shared_counter;
            }
        }));
    }

    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index].join();
    }

    assert(shared_counter == thread_count * increments_per_thread);
}

void test_mutex_try_lock() {
    // 验证 try_lock 不阻塞，并且锁被占用时会立即失败。
    concurrency::Mutex mutex;

    assert(mutex.try_lock());
    assert(!mutex.try_lock());
    mutex.unlock();
    assert(mutex.try_lock());
    mutex.unlock();
}

void test_shared_mutex_allows_multiple_readers() {
    // shared_mutex 用来验证多个读线程能同时持有共享锁。
    concurrency::SharedMutex shared_mutex;

    // concurrent_readers 记录当前正在读临界区内的线程数量。
    std::atomic<int> concurrent_readers(0);

    // max_concurrent_readers 记录测试过程中观察到的最大并发读者数量。
    std::atomic<int> max_concurrent_readers(0);

    // 启动多个读线程，期望至少有两个读线程能重叠进入临界区。
    const int reader_count = 6;
    std::vector<std::thread> readers;

    for (int reader_index = 0; reader_index < reader_count; ++reader_index) {
        readers.push_back(std::thread([&]() {
            concurrency::SharedLockGuard<concurrency::SharedMutex> read_guard(shared_mutex);

            // 当前线程进入读临界区，读者数量加 1。
            const int readers_now = concurrent_readers.fetch_add(1) + 1;

            // 使用 CAS 更新最大并发读者数量，避免测试统计变量自身产生数据竞争。
            int observed_max = max_concurrent_readers.load();
            while (readers_now > observed_max &&
                   !max_concurrent_readers.compare_exchange_weak(observed_max, readers_now)) {
            }

            // 短暂停留在读临界区内，让多个读线程有机会重叠。
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            concurrent_readers.fetch_sub(1);
        }));
    }

    for (std::size_t index = 0; index < readers.size(); ++index) {
        readers[index].join();
    }

    assert(max_concurrent_readers.load() > 1);
}

void test_shared_mutex_writer_is_exclusive() {
    // shared_mutex 用来保护 protected_value 的写入。
    concurrency::SharedMutex shared_mutex;

    // protected_value 只能在持有独占写锁时修改。
    int protected_value = 0;

    // 多个写线程同时竞争独占写锁。
    const int writer_count = 4;
    const int writes_per_thread = 5000;
    std::vector<std::thread> writers;

    for (int writer_index = 0; writer_index < writer_count; ++writer_index) {
        writers.push_back(std::thread([&]() {
            for (int step = 0; step < writes_per_thread; ++step) {
                // write_guard 保证同一时刻只有一个写线程能递增 protected_value。
                concurrency::UniqueLockGuard<concurrency::SharedMutex> write_guard(shared_mutex);
                ++protected_value;
            }
        }));
    }

    for (std::size_t index = 0; index < writers.size(); ++index) {
        writers[index].join();
    }

    assert(protected_value == writer_count * writes_per_thread);
}

void test_shared_mutex_try_lock() {
    // 验证 try_lock_shared 可以允许多个读者，但会阻止写锁进入。
    concurrency::SharedMutex shared_mutex;

    assert(shared_mutex.try_lock_shared());
    assert(shared_mutex.try_lock_shared());
    assert(!shared_mutex.try_lock());
    shared_mutex.unlock_shared();
    shared_mutex.unlock_shared();

    assert(shared_mutex.try_lock());
    assert(!shared_mutex.try_lock_shared());
    assert(!shared_mutex.try_lock());
    shared_mutex.unlock();
}

} // namespace

int main() {
    test_mutex_protects_counter();
    test_mutex_try_lock();
    test_shared_mutex_allows_multiple_readers();
    test_shared_mutex_writer_is_exclusive();
    test_shared_mutex_try_lock();
    return 0;
}
