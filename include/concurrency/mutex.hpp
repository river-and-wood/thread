#ifndef CONCURRENCY_MUTEX_HPP
#define CONCURRENCY_MUTEX_HPP

#include <atomic>

#include "concurrency/noncopyable.hpp"

namespace concurrency {

// 一个简单的自旋互斥锁。
//
// 适合展示 C++11 原子操作和 acquire/release 内存序。
// 注意：自旋锁适合临界区很短的场景；临界区较长时会浪费 CPU。
//
// 状态模型：
// - locked_ == false：锁空闲，线程可以尝试进入临界区。
// - locked_ == true ：锁被占用，其他线程需要等待。
class Mutex : private NonCopyable {
public:
    Mutex();

    // 阻塞直到拿到独占锁。
    void lock();

    // 尝试获取独占锁。
    // 返回 true 表示获取成功，返回 false 表示锁已经被其他线程持有。
    bool try_lock();

    // 释放独占锁。
    void unlock();

private:
    // atomic_flag 是 C++11 提供的最小原子布尔标志。
    // false 表示当前没有线程持锁，true 表示已经有线程持锁。
    // 它是本互斥锁唯一的共享状态，所有线程都通过它竞争锁所有权。
    std::atomic_flag locked_;
};

} // namespace concurrency

#endif // CONCURRENCY_MUTEX_HPP
