#ifndef CONCURRENCY_THREAD_POOL_HPP
#define CONCURRENCY_THREAD_POOL_HPP

#include "concurrency/mutex.hpp"
#include "concurrency/noncopyable.hpp"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace concurrency {

// C++11 固定大小线程池。
//
// 任务提交后进入共享队列，由工作线程取出执行。
// enqueue() 返回 std::future，调用方可以获取返回值或接收任务异常。
// 析构时会停止接收新任务，并等待已提交任务执行完成。
//
// 设计边界：
// - 线程数量在构造时确定，运行期间不动态扩缩容。
// - 任务队列是 FIFO，所有工作线程共享同一个队列。
// - 队列同步使用本项目自研的 Mutex，阻塞等待使用 condition_variable_any。
//   std::condition_variable 只能配合 std::mutex 使用；
//   condition_variable_any 可以配合满足 BasicLockable 语义的自定义锁使用。
// - 线程池析构表示“关闭入口”，不是强制取消任务：
//   已经放进队列的任务会继续执行完，新的 enqueue() 会被拒绝。
class ThreadPool : private NonCopyable {
public:
    // 创建 thread_count 个工作线程。
    // thread_count 必须大于 0，否则线程池没有执行任务的消费者。
    explicit ThreadPool(std::size_t thread_count);

    // 析构时通知所有 worker 退出，并 join 等待它们结束。
    // 因为 worker_loop 会先处理完 tasks_ 中的任务，所以这里是优雅退出。
    ~ThreadPool();

    // 提交任务到线程池。
    //
    // Function 可以是普通函数、lambda、函数对象、成员函数包装后的可调用对象。
    // Args 是传给 Function 的参数。
    //
    // 返回值类型使用 std::result_of 在 C++11 中推导：
    // - 如果任务返回 int，则返回 std::future<int>。
    // - 如果任务返回 void，则返回 std::future<void>。
    //
    // future 的作用：
    // - 调用 get() 可以等待任务执行完成。
    // - 任务返回值会通过 get() 取出。
    // - 任务内部抛出的异常会被 packaged_task 捕获，并在 get() 时重新抛出。
    template <typename Function, typename... Args>
    auto enqueue(Function&& function, Args&&... args)
        -> std::future<typename std::result_of<Function(Args...)>::type>;

private:
    // 线程池内部队列只保存“无参数、无返回值”的统一任务类型。
    // enqueue() 会把任意签名的任务包装成 Task，worker 只需要调用 task()。
    typedef std::function<void()> Task;

    // 每个工作线程运行的主循环：
    // 等待任务或停止信号，取出一个任务，在锁外执行。
    void worker_loop();

    // 持有所有工作线程对象，析构时逐个 join。
    std::vector<std::thread> workers_;

    // 共享任务队列。多个提交线程会 push，多个 worker 会 pop，
    // 所有访问都必须在 queue_mutex_ 保护下进行。
    std::queue<Task> tasks_;

    // 保护 tasks_ 和 stopping_ 的互斥锁。
    // 这里使用项目自研 Mutex，而不是 std::mutex，用来展示自研锁在真实组件中的使用。
    Mutex queue_mutex_;

    // 条件变量负责让 worker 在没有任务时阻塞睡眠。
    // 使用 condition_variable_any 是因为 queue_mutex_ 不是 std::mutex。
    std::condition_variable_any condition_;

    // 关闭标志。
    // false：线程池正常接收任务。
    // true ：线程池正在关闭，不再接收新任务；worker 在队列清空后退出。
    // 该变量受 queue_mutex_ 保护，不单独使用 atomic。
    bool stopping_;
};

template <typename Function, typename... Args>
auto ThreadPool::enqueue(Function&& function, Args&&... args)
    -> std::future<typename std::result_of<Function(Args...)>::type> {
    // ReturnType 是任务真实返回值类型。
    // 例如 enqueue([] { return 1; }) 的 ReturnType 是 int。
    typedef typename std::result_of<Function(Args...)>::type ReturnType;

    // packaged_task 把一个可调用对象包装成“可异步获取结果”的任务。
    //
    // std::bind 将 function 和 args 绑定成一个无参数函数：
    // worker 线程之后只需要执行 (*task)()，不需要知道原始参数。
    //
    // 使用 shared_ptr 的原因：
    // - std::packaged_task 是 move-only，不能直接复制进 std::function<void()>。
    // - lambda 捕获 shared_ptr 后可以复制，满足 std::function 的要求。
    // - 实际 packaged_task 对象仍然只有一个共享实例。
    std::shared_ptr<std::packaged_task<ReturnType()> > task(
        new std::packaged_task<ReturnType()>(
            std::bind(std::forward<Function>(function),
                      std::forward<Args>(args)...)));

    // future 必须在任务执行前取出。
    // 调用方拿到 future 后，可以在任意时刻 get() 等待任务完成。
    std::future<ReturnType> result = task->get_future();

    {
        // unique_lock 用来配合 condition_variable_any。
        // 它会在作用域结束时自动 unlock queue_mutex_。
        std::unique_lock<Mutex> lock(queue_mutex_);

        // stopping_ 为 true 表示析构流程已经开始。
        // 此时继续接收新任务会导致“析构永远等新任务”的语义混乱，所以直接拒绝。
        if (stopping_) {
            throw std::runtime_error("cannot enqueue task after ThreadPool shutdown");
        }

        // 队列中保存统一的 void() 任务。
        // worker 调用这个 lambda 时，内部 packaged_task 会执行真实任务，
        // 并把返回值或异常保存到对应的 future 状态里。
        tasks_.push([task]() {
            (*task)();
        });
    }

    // 任务入队后唤醒一个 worker。
    // notify_one 不需要持锁调用；这里先释放锁再通知，减少被唤醒线程立刻抢锁失败的概率。
    condition_.notify_one();
    return result;
}

} // namespace concurrency

#endif // CONCURRENCY_THREAD_POOL_HPP
