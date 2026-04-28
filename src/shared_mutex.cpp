#include "concurrency/shared_mutex.hpp"

#include <thread>

namespace concurrency {

SharedMutex::SharedMutex()
    : state_(0),
      waiting_writers_(0) {
    // state_ = 0 表示初始时没有读线程，也没有写线程。
    // waiting_writers_ = 0 表示初始时没有等待写锁的线程。
}

void SharedMutex::lock() {
    // 当前线程准备申请写锁，先登记为等待写线程。
    // 新来的读线程看到 waiting_writers_ > 0 后会暂缓进入，
    // 这样可以降低写线程被连续读线程饿死的概率。
    waiting_writers_.fetch_add(1, std::memory_order_acq_rel);

    for (;;) {
        // CAS 的期望值。只有 state_ 仍然等于 0，CAS 才会成功。
        // 每一轮都要重新设置为 0，因为 CAS 失败时会把真实 state_ 写回该变量。
        int expected_state = 0;

        // 只有 state_ 为 0 时，写线程才能把状态改成 -1。
        // compare_exchange_weak 允许虚假失败，所以必须放在循环里重试。
        if (state_.compare_exchange_weak(expected_state,
                                         -1,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed)) {
            // 已经成功拿到写锁，不再属于“等待写锁”的线程。
            waiting_writers_.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }

        // 当前存在读线程或写线程，或者 CAS 虚假失败；让出时间片后继续重试。
        std::this_thread::yield();
    }
}

bool SharedMutex::try_lock() {
    // try_lock 只尝试一次，不进入循环等待。
    // expected_state 被设置为 0，表示只有完全空闲时才能拿到写锁。
    int expected_state = 0;

    // 只有无人持锁时，才能把 state_ 从 0 改成 -1。
    return state_.compare_exchange_strong(expected_state,
                                          -1,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed);
}

void SharedMutex::unlock() {
    // 写线程释放锁时，把 -1 改回 0。
    // release 发布写临界区内对共享数据的修改。
    state_.store(0, std::memory_order_release);
}

void SharedMutex::lock_shared() {
    for (;;) {
        // 简单写优先策略：有写线程等待时，新的读线程暂缓进入。
        if (waiting_writers_.load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
            continue;
        }

        // current_state 是当前观察到的读写锁状态。
        // -1 表示写锁占用；0 或正数表示没有写线程持锁。
        int current_state = state_.load(std::memory_order_acquire);

        // current_state < 0 表示写线程正在持有独占锁。
        if (current_state < 0) {
            std::this_thread::yield();
            continue;
        }

        // 尝试把读线程数量加 1。
        // 如果期间状态被其他线程改了，CAS 会失败并重新读取状态。
        if (state_.compare_exchange_weak(current_state,
                                         current_state + 1,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed)) {
            // CAS 成功后，当前线程已经计入读者数量，可以进入共享临界区。
            return;
        }

        // 可能有其他读线程同时修改了读者数量，当前线程重新尝试。
        std::this_thread::yield();
    }
}

bool SharedMutex::try_lock_shared() {
    // 有写线程正在等待时，新的读线程不插队。
    if (waiting_writers_.load(std::memory_order_acquire) > 0) {
        return false;
    }

    // current_state 会在 CAS 失败时被更新为 state_ 的真实值。
    // while 条件保证只要观察到写锁状态 -1，就立刻失败退出。
    int current_state = state_.load(std::memory_order_acquire);

    while (current_state >= 0) {
        // 尝试把读者数量加 1。
        if (state_.compare_exchange_weak(current_state,
                                         current_state + 1,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed)) {
            return true;
        }
    }

    return false;
}

void SharedMutex::unlock_shared() {
    // 读线程释放共享锁时，把读者数量减 1。
    // 如果当前线程是最后一个读线程，state_ 会从 1 变成 0。
    state_.fetch_sub(1, std::memory_order_release);
}

} // namespace concurrency
