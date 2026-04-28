#include "concurrency/lock_guard.hpp"
#include "concurrency/mutex.hpp"
#include "concurrency/shared_mutex.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace {

void wait_for_start(std::atomic<int>& ready_threads,
                    std::atomic<bool>& start_signal) {
    // ready_threads 记录已经准备好的线程数量。
    ready_threads.fetch_add(1, std::memory_order_acq_rel);

    // start_signal 为 false 时，所有线程在这里等待，尽量同时开始竞争锁。
    while (!start_signal.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void update_maximum(std::atomic<int>& maximum_value, int candidate_value) {
    // maximum_value 是多个线程共享的最大值统计。
    // CAS 失败时 observed_value 会被更新为当前真实值，所以循环可以继续比较。
    int observed_value = maximum_value.load(std::memory_order_acquire);
    while (candidate_value > observed_value &&
           !maximum_value.compare_exchange_weak(observed_value,
                                                candidate_value,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
    }
}

void stress_mutex_counter() {
    concurrency::Mutex counter_mutex;
    long long shared_counter = 0;

    const int thread_count = 12;
    const int increments_per_thread = 50000;

    std::atomic<int> ready_threads(0);
    std::atomic<bool> start_signal(false);
    std::vector<std::thread> workers;

    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        workers.push_back(std::thread([&]() {
            wait_for_start(ready_threads, start_signal);

            for (int step = 0; step < increments_per_thread; ++step) {
                concurrency::LockGuard<concurrency::Mutex> guard(counter_mutex);
                ++shared_counter;
            }
        }));
    }

    while (ready_threads.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }
    start_signal.store(true, std::memory_order_release);

    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index].join();
    }

    assert(shared_counter == static_cast<long long>(thread_count) * increments_per_thread);
}

void stress_mutex_try_lock() {
    concurrency::Mutex counter_mutex;
    long long shared_counter = 0;

    const int thread_count = 10;
    const int increments_per_thread = 30000;

    std::atomic<int> ready_threads(0);
    std::atomic<bool> start_signal(false);
    std::atomic<int> try_lock_failures(0);
    std::vector<std::thread> workers;

    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        workers.push_back(std::thread([&]() {
            wait_for_start(ready_threads, start_signal);

            for (int step = 0; step < increments_per_thread; ++step) {
                if (counter_mutex.try_lock()) {
                    ++shared_counter;
                    counter_mutex.unlock();
                } else {
                    // try_lock 失败后走阻塞 lock 路径，保证每次循环最终都会完成递增。
                    try_lock_failures.fetch_add(1, std::memory_order_acq_rel);
                    concurrency::LockGuard<concurrency::Mutex> guard(counter_mutex);
                    ++shared_counter;
                }
            }
        }));
    }

    while (ready_threads.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }
    start_signal.store(true, std::memory_order_release);

    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index].join();
    }

    assert(shared_counter == static_cast<long long>(thread_count) * increments_per_thread);
    assert(try_lock_failures.load(std::memory_order_acquire) > 0);
}

void stress_shared_mutex_mixed_read_write() {
    concurrency::SharedMutex shared_mutex;

    // 这三个变量必须在写锁保护下同时更新，在读锁保护下同时读取。
    long long protected_value = 0;
    long long protected_mirror = 0;
    long long protected_double = 0;

    // active_readers/active_writers 用来检查读写锁是否真的满足：
    // 多读可以并发，写必须独占，读写不能重叠。
    std::atomic<int> active_readers(0);
    std::atomic<int> active_writers(0);
    std::atomic<int> max_parallel_readers(0);
    std::atomic<int> invariant_failures(0);

    const int reader_count = 8;
    const int writer_count = 4;
    const int reader_rounds = 25000;
    const int writer_rounds = 8000;
    const int total_threads = reader_count + writer_count;

    std::atomic<int> ready_threads(0);
    std::atomic<bool> start_signal(false);
    std::vector<std::thread> threads;

    for (int reader_index = 0; reader_index < reader_count; ++reader_index) {
        threads.push_back(std::thread([&]() {
            wait_for_start(ready_threads, start_signal);

            for (int round = 0; round < reader_rounds; ++round) {
                concurrency::SharedLockGuard<concurrency::SharedMutex> guard(shared_mutex);

                const int readers_now = active_readers.fetch_add(1, std::memory_order_acq_rel) + 1;
                update_maximum(max_parallel_readers, readers_now);

                if (active_writers.load(std::memory_order_acquire) != 0) {
                    invariant_failures.fetch_add(1, std::memory_order_acq_rel);
                }

                const long long value_snapshot = protected_value;
                const long long mirror_snapshot = protected_mirror;
                const long long double_snapshot = protected_double;

                if (mirror_snapshot != -value_snapshot ||
                    double_snapshot != value_snapshot * 2) {
                    invariant_failures.fetch_add(1, std::memory_order_acq_rel);
                }

                if ((round % 64) == 0) {
                    std::this_thread::yield();
                }

                active_readers.fetch_sub(1, std::memory_order_acq_rel);
            }
        }));
    }

    for (int writer_index = 0; writer_index < writer_count; ++writer_index) {
        threads.push_back(std::thread([&]() {
            wait_for_start(ready_threads, start_signal);

            for (int round = 0; round < writer_rounds; ++round) {
                concurrency::UniqueLockGuard<concurrency::SharedMutex> guard(shared_mutex);

                const int writers_now = active_writers.fetch_add(1, std::memory_order_acq_rel) + 1;

                if (writers_now != 1 ||
                    active_readers.load(std::memory_order_acquire) != 0) {
                    invariant_failures.fetch_add(1, std::memory_order_acq_rel);
                }

                ++protected_value;
                protected_mirror = -protected_value;
                protected_double = protected_value * 2;

                if ((round % 32) == 0) {
                    std::this_thread::yield();
                }

                active_writers.fetch_sub(1, std::memory_order_acq_rel);
            }
        }));
    }

    while (ready_threads.load(std::memory_order_acquire) != total_threads) {
        std::this_thread::yield();
    }
    start_signal.store(true, std::memory_order_release);

    for (std::size_t index = 0; index < threads.size(); ++index) {
        threads[index].join();
    }

    assert(invariant_failures.load(std::memory_order_acquire) == 0);
    assert(max_parallel_readers.load(std::memory_order_acquire) > 1);
    assert(protected_value == static_cast<long long>(writer_count) * writer_rounds);
    assert(protected_mirror == -protected_value);
    assert(protected_double == protected_value * 2);
}

void stress_shared_mutex_try_lock_shared() {
    concurrency::SharedMutex shared_mutex;

    int protected_value = 0;
    std::atomic<int> successful_reads(0);
    std::atomic<int> failed_reads(0);
    std::atomic<int> successful_writes(0);

    const int thread_count = 8;
    const int rounds_per_thread = 20000;

    std::atomic<int> ready_threads(0);
    std::atomic<bool> start_signal(false);
    std::vector<std::thread> workers;

    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        workers.push_back(std::thread([&, thread_index]() {
            wait_for_start(ready_threads, start_signal);

            for (int round = 0; round < rounds_per_thread; ++round) {
                if ((round + thread_index) % 5 == 0) {
                    concurrency::UniqueLockGuard<concurrency::SharedMutex> guard(shared_mutex);
                    ++protected_value;
                    successful_writes.fetch_add(1, std::memory_order_acq_rel);
                } else if (shared_mutex.try_lock_shared()) {
                    const int snapshot = protected_value;
                    assert(snapshot >= 0);
                    successful_reads.fetch_add(1, std::memory_order_acq_rel);
                    shared_mutex.unlock_shared();
                } else {
                    failed_reads.fetch_add(1, std::memory_order_acq_rel);
                    concurrency::SharedLockGuard<concurrency::SharedMutex> guard(shared_mutex);
                    const int snapshot = protected_value;
                    assert(snapshot >= 0);
                }
            }
        }));
    }

    while (ready_threads.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }
    start_signal.store(true, std::memory_order_release);

    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index].join();
    }

    assert(protected_value == successful_writes.load(std::memory_order_acquire));
    assert(successful_reads.load(std::memory_order_acquire) > 0);
    assert(failed_reads.load(std::memory_order_acquire) > 0);
}

} // namespace

int main() {
    const std::chrono::steady_clock::time_point started_at =
        std::chrono::steady_clock::now();

    stress_mutex_counter();
    stress_mutex_try_lock();
    stress_shared_mutex_mixed_read_write();
    stress_shared_mutex_try_lock_shared();

    const std::chrono::steady_clock::time_point finished_at =
        std::chrono::steady_clock::now();
    const std::chrono::milliseconds elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(finished_at - started_at);

    std::cout << "stress_tests passed in " << elapsed.count() << " ms" << std::endl;
    return 0;
}
