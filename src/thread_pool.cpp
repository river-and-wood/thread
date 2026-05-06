#include "concurrency/thread_pool.hpp"

#include <stdexcept>

namespace concurrency {

ThreadPool::ThreadPool(std::size_t thread_count)
    : stopping_(false) {
    // 线程池至少需要一个 worker。
    // 如果允许 0 个线程，enqueue() 的任务永远不会被消费，future::get() 会永久等待。
    if (thread_count == 0) {
        throw std::invalid_argument("ThreadPool requires at least one worker");
    }

    try {
        // 创建固定数量的工作线程。
        // 每个线程都执行同一个 worker_loop，通过共享任务队列领取任务。
        for (std::size_t index = 0; index < thread_count; ++index) {
            workers_.push_back(std::thread(&ThreadPool::worker_loop, this));
        }
    } catch (...) {
        // 如果创建部分线程后又创建失败，需要把已经创建的线程安全收回。
        // 这里进入关闭状态，避免已启动 worker 继续等待永远不会完成的构造过程。
        {
            std::unique_lock<Mutex> lock(queue_mutex_);
            stopping_ = true;
        }

        // 唤醒所有已经启动的 worker，让它们看到 stopping_ 后退出。
        condition_.notify_all();

        // join 已经成功创建的线程，避免构造函数抛异常时留下后台线程。
        for (std::size_t index = 0; index < workers_.size(); ++index) {
            if (workers_[index].joinable()) {
                workers_[index].join();
            }
        }

        throw;
    }
}

ThreadPool::~ThreadPool() {
    {
        // 析构时先设置停止标志。
        // 这个标志和任务队列共用同一把锁，保证 worker 看到一致状态：
        // 要么还有任务可取，要么队列为空且可以退出。
        std::unique_lock<Mutex> lock(queue_mutex_);
        stopping_ = true;
    }

    // 唤醒所有 worker。
    // 如果某些 worker 正阻塞在 condition_.wait()，它们需要被唤醒后才能检查 stopping_。
    // 使用 notify_all 是因为析构需要所有线程都有机会退出。
    condition_.notify_all();

    // 等待每个 worker 线程结束。
    // join 完成后，线程池对象销毁才是安全的，因为 worker_loop 不再访问 this。
    for (std::size_t index = 0; index < workers_.size(); ++index) {
        if (workers_[index].joinable()) {
            workers_[index].join();
        }
    }
}

void ThreadPool::worker_loop() {
    for (;;) {
        // task 定义在锁外层。
        // 这样可以先从队列取出任务并释放锁，再执行任务，避免任务执行期间阻塞其他线程入队/出队。
        Task task;

        {
            std::unique_lock<Mutex> lock(queue_mutex_);

            // 没有任务时，worker 阻塞睡眠，而不是自旋消耗 CPU。
            //
            // wait(lock, predicate) 等价于循环：
            // while (!predicate()) wait(lock);
            // 这样可以正确处理条件变量的虚假唤醒。
            //
            // wait 内部会在阻塞前释放 queue_mutex_，被唤醒后重新加锁，
            // 所以其他线程可以在 worker 睡眠期间 enqueue() 新任务。
            condition_.wait(lock, [this]() {
                return stopping_ || !tasks_.empty();
            });

            // 退出条件必须同时满足：
            // - stopping_ 为 true：线程池进入关闭流程。
            // - tasks_ 为空：已经没有已提交任务需要执行。
            //
            // 如果 stopping_ 为 true 但队列不空，worker 仍然继续取任务执行，
            // 这就是“析构时完成已排队任务”的优雅退出语义。
            if (stopping_ && tasks_.empty()) {
                return;
            }

            // 从队列头部取出一个任务。
            // std::queue::front() 返回引用，必须在 pop() 前复制到局部变量。
            task = tasks_.front();
            tasks_.pop();
        }

        // 在不持有 queue_mutex_ 的情况下执行任务。
        // 这样长任务不会阻塞其他线程继续 enqueue，也不会阻塞其他 worker 取任务。
        // packaged_task 会把返回值或异常写入对应 future。
        task();
    }
}

} // namespace concurrency
