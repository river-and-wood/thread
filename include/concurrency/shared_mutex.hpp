#ifndef CONCURRENCY_SHARED_MUTEX_HPP
#define CONCURRENCY_SHARED_MUTEX_HPP

#include <atomic>

#include "concurrency/noncopyable.hpp"

namespace concurrency {

// C++11 版本的自旋读写锁。
//
// 多个读线程可以同时进入共享临界区。
// 写线程需要独占进入，进入时不能有其他读线程或写线程。
//
// 这个实现不封装 std::mutex/std::shared_mutex/std::condition_variable，
// 只使用 std::atomic 作为底层原子操作基础。
//
// 状态模型：
// - 读线程只要没有写线程持锁，就可以共同进入。
// - 写线程必须等到没有读线程、也没有其他写线程时才能进入。
class SharedMutex : private NonCopyable {
public:
    SharedMutex();

    // 获取独占写锁。
    void lock();

    // 尝试获取独占写锁。
    // 返回 true 表示获取成功，返回 false 表示当前存在读线程或写线程。
    bool try_lock();

    // 释放独占写锁。
    void unlock();

    // 获取共享读锁。
    void lock_shared();

    // 尝试获取共享读锁。
    // 返回 true 表示获取成功，返回 false 表示当前有写线程活跃或等待。
    bool try_lock_shared();

    // 释放共享读锁。
    void unlock_shared();

private:
    // 读写锁的核心状态。
    // -1 表示当前有一个写线程持有独占锁。
    //  0 表示当前没有线程持锁。
    // >0 表示当前持有共享读锁的线程数量。
    //
    // 把“是否有写线程”和“读线程数量”压缩在一个原子整数里，
    // 可以用 CAS 一次性完成状态检查和状态修改，避免多个变量不一致。
    std::atomic<int> state_;

    // 正在等待写锁的线程数量。
    // 读线程会观察这个值：只要存在等待写线程，新的读线程先不进入，
    // 这样可以降低写线程长时间饥饿的概率。
    //
    // 这是一个简化写优先策略，不是严格 FIFO 公平队列。
    std::atomic<int> waiting_writers_;
};

} // namespace concurrency

#endif // CONCURRENCY_SHARED_MUTEX_HPP
