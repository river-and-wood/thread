#include "concurrency/thread_pool.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

int add(int left, int right) {
    return left + right;
}

void test_thread_pool_returns_values() {
    // 验证 enqueue() 可以接受普通函数和 lambda，
    // 并通过 future 把任务返回值传回调用线程。
    concurrency::ThreadPool pool(4);

    std::future<int> first = pool.enqueue(add, 2, 3);
    std::future<int> second = pool.enqueue([](int value) {
        return value * value;
    }, 7);

    assert(first.get() == 5);
    assert(second.get() == 49);
}

void test_thread_pool_runs_tasks_in_parallel() {
    // 验证任务不是在调用 enqueue() 的线程里同步执行，
    // 而是由多个 worker 并行消费任务队列。
    concurrency::ThreadPool pool(4);

    // active_tasks 表示当前正在执行任务的数量。
    // max_active_tasks 记录测试期间观察到的最大并行任务数。
    std::atomic<int> active_tasks(0);
    std::atomic<int> max_active_tasks(0);

    // 保存 future，确保所有任务执行完成后再检查结果。
    std::vector<std::future<void> > futures;

    for (int index = 0; index < 8; ++index) {
        futures.push_back(pool.enqueue([&]() {
            // 进入任务时把活跃任务数加 1。
            // 如果线程池真的并行执行，max_active_tasks 应该大于 1。
            const int active_now = active_tasks.fetch_add(1, std::memory_order_acq_rel) + 1;

            // 用 CAS 更新最大值，避免多个任务同时更新统计变量造成数据竞争。
            int observed_max = max_active_tasks.load(std::memory_order_acquire);
            while (active_now > observed_max &&
                   !max_active_tasks.compare_exchange_weak(observed_max,
                                                           active_now,
                                                           std::memory_order_acq_rel,
                                                           std::memory_order_relaxed)) {
            }

            // 保持任务短暂运行，让多个 worker 有机会重叠执行。
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            active_tasks.fetch_sub(1, std::memory_order_acq_rel);
        }));
    }

    // get() 会等待任务完成。
    // 对 std::future<void> 来说，get() 没有返回值，但仍会传播任务异常。
    for (std::size_t index = 0; index < futures.size(); ++index) {
        futures[index].get();
    }

    assert(max_active_tasks.load(std::memory_order_acquire) > 1);
}

void test_thread_pool_propagates_exceptions() {
    // 验证任务内部抛出的异常不会让 worker 线程直接崩溃，
    // 而是被 packaged_task 捕获，并在 future::get() 时重新抛给调用方。
    concurrency::ThreadPool pool(2);

    std::future<void> result = pool.enqueue([]() {
        throw std::runtime_error("task failed");
    });

    bool caught_exception = false;
    try {
        result.get();
    } catch (const std::runtime_error&) {
        caught_exception = true;
    }

    assert(caught_exception);
}

void test_thread_pool_destructor_finishes_queued_tasks() {
    // 验证析构不是“丢弃任务”或“强行终止线程”，
    // 而是停止接收新任务，并等待队列中已有任务全部完成。
    std::atomic<int> completed_tasks(0);

    {
        concurrency::ThreadPool pool(3);
        for (int index = 0; index < 30; ++index) {
            pool.enqueue([&]() {
                // 轻微延迟让任务更可能在析构发生时仍有排队或执行中的状态。
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                completed_tasks.fetch_add(1, std::memory_order_acq_rel);
            });
        }
    }

    assert(completed_tasks.load(std::memory_order_acquire) == 30);
}

void test_thread_pool_rejects_zero_workers() {
    // 验证非法线程数会被拒绝。
    // 0 worker 会导致提交的任务无人消费，因此构造函数直接抛异常。
    bool caught_exception = false;

    try {
        concurrency::ThreadPool pool(0);
    } catch (const std::invalid_argument&) {
        caught_exception = true;
    }

    assert(caught_exception);
}

} // namespace

int main() {
    test_thread_pool_returns_values();
    test_thread_pool_runs_tasks_in_parallel();
    test_thread_pool_propagates_exceptions();
    test_thread_pool_destructor_finishes_queued_tasks();
    test_thread_pool_rejects_zero_workers();
    return 0;
}
