#include "concurrency/mutex.hpp"

#include <thread>

namespace concurrency {

Mutex::Mutex()
    : locked_(ATOMIC_FLAG_INIT) {
    // ATOMIC_FLAG_INIT 将 locked_ 初始化为空闲状态。
}

void Mutex::lock() {
    // test_and_set 会把 locked_ 设置为 true，并返回设置前的旧值。
    // 返回 true 表示已经有线程持锁，当前线程继续自旋等待。
    while (locked_.test_and_set(std::memory_order_acquire)) {
        // 主动让出时间片，降低忙等对 CPU 的压力。
        // yield 不是阻塞等待，只是给调度器一个让出 CPU 的提示。
        std::this_thread::yield();
    }
}

bool Mutex::try_lock() {
    // test_and_set 返回设置前的旧值。
    // 旧值为 false，说明当前线程成功把锁状态改成 true。
    return !locked_.test_and_set(std::memory_order_acquire);
}

void Mutex::unlock() {
    // release 保证临界区内的写入对之后拿到锁的线程可见。
    // clear 将 locked_ 从 true 恢复为 false，表示锁重新空闲。
    locked_.clear(std::memory_order_release);
}

} // namespace concurrency
