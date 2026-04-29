#include "concurrency/shared_mutex.hpp"

#include <thread>

namespace concurrency {

SharedMutex::SharedMutex()
    : state_(0),
      next_ticket_(0),
      serving_ticket_(0) {
    // state_ = 0 表示初始时没有读线程，也没有写线程。
    // next_ticket_ 和 serving_ticket_ 从 0 开始，表示还没有任何排队请求。
}

void SharedMutex::lock() {
    // 写线程领取全局统一 ticket。
    // 读请求和写请求都从 next_ticket_ 取号，因此不会互相插队。
    const unsigned long long my_ticket =
        next_ticket_.fetch_add(1, std::memory_order_acq_rel);

    for (;;) {
        // 只有轮到自己的 ticket 时，写线程才允许竞争 state_。
        if (serving_ticket_.load(std::memory_order_acquire) != my_ticket) {
            std::this_thread::yield();
            continue;
        }

        // CAS 的期望值。只有 state_ 仍然等于 0，CAS 才会成功。
        // 每一轮都要重新设置为 0，因为 CAS 失败时会把真实 state_ 写回该变量。
        int expected_state = 0;

        // 只有 state_ 为 0 时，写线程才能把状态改成 -1。
        // compare_exchange_weak 允许虚假失败，所以必须放在循环里重试。
        if (state_.compare_exchange_weak(expected_state,
                                         -1,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed)) {
            // 写线程持有 ticket，直到 unlock() 时才推进 serving_ticket_。
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

    // try_lock 也必须遵守统一 ticket。
    // 只有 next_ticket_ == serving_ticket_ 时，才说明当前没有排队请求。
    const unsigned long long serving_ticket =
        serving_ticket_.load(std::memory_order_acquire);
    unsigned long long expected_next_ticket = serving_ticket;

    // 预留当前 serving ticket。预留失败说明已有请求排队，当前 try_lock 不能插队。
    if (!next_ticket_.compare_exchange_strong(expected_next_ticket,
                                             serving_ticket + 1,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
        return false;
    }

    // 只有无人持锁时，才能把 state_ 从 0 改成 -1。
    const bool locked = state_.compare_exchange_strong(expected_state,
                                                       -1,
                                                       std::memory_order_acquire,
                                                       std::memory_order_relaxed);

    if (!locked) {
        // 已经预留了一个 ticket，但没有拿到锁。
        // 必须推进 serving_ticket_ 跳过这个未使用的 ticket，
        // 否则后续写线程会永远等在这个 ticket 上。
        serving_ticket_.fetch_add(1, std::memory_order_acq_rel);
    }

    return locked;
}

void SharedMutex::unlock() {
    // 写线程释放锁时，把 -1 改回 0。
    // release 发布写临界区内对共享数据的修改。
    state_.store(0, std::memory_order_release);

    // 写线程持有一个统一 ticket。
    // 释放写锁后推进 ticket，让下一个写线程有资格竞争。
    serving_ticket_.fetch_add(1, std::memory_order_acq_rel);
}

void SharedMutex::lock_shared() {
    // 读线程同样领取全局统一 ticket。
    // 如果它排在某个写线程后面，就必须等待该写线程完成。
    const unsigned long long my_ticket =
        next_ticket_.fetch_add(1, std::memory_order_acq_rel);

    for (;;) {
        // 未轮到自己的 ticket 时不能进入。
        if (serving_ticket_.load(std::memory_order_acquire) != my_ticket) {
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
            // 读线程进入后立即推进 serving_ticket_，允许后续连续读线程形成并发读批次。
            // 如果后续是写线程，它会看到 state_ > 0，并等待所有读线程退出。
            serving_ticket_.fetch_add(1, std::memory_order_acq_rel);
            return;
        }

        // 可能有其他读线程同时修改了读者数量，当前线程重新尝试。
        std::this_thread::yield();
    }
}

bool SharedMutex::try_lock_shared() {
    // try_lock_shared 也不能插队。
    // 只有没有任何排队请求时，它才尝试预留当前 serving ticket。
    const unsigned long long serving_ticket =
        serving_ticket_.load(std::memory_order_acquire);
    unsigned long long expected_next_ticket = serving_ticket;

    if (!next_ticket_.compare_exchange_strong(expected_next_ticket,
                                             serving_ticket + 1,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
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
            // 读线程进入后马上推进 ticket，让后续请求继续按 FIFO 检查。
            serving_ticket_.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }
    }

    // 已经预留了 ticket，但当前有写线程持锁，必须跳过这个 ticket。
    serving_ticket_.fetch_add(1, std::memory_order_acq_rel);
    return false;
}

void SharedMutex::unlock_shared() {
    // 读线程释放共享锁时，把读者数量减 1。
    // 如果当前线程是最后一个读线程，state_ 会从 1 变成 0。
    state_.fetch_sub(1, std::memory_order_release);
}

} // namespace concurrency
