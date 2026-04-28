#ifndef CONCURRENCY_LOCK_GUARD_HPP
#define CONCURRENCY_LOCK_GUARD_HPP

#include "concurrency/noncopyable.hpp"

namespace concurrency {

// 独占互斥锁 RAII 守卫。
// 构造函数加锁，析构函数自动解锁，避免异常或提前 return 导致锁泄漏。
template <typename LockType>
class LockGuard : private NonCopyable {
public:
    explicit LockGuard(LockType& lock)
        : lock_(lock) {
        // 进入作用域时获取独占锁。
        lock_.lock();
    }

    ~LockGuard() {
        // 离开作用域时自动释放锁。
        lock_.unlock();
    }

private:
    // 被管理的锁对象引用，生命周期由调用方保证。
    LockType& lock_;
};

// 读写锁的共享读守卫。
template <typename SharedMutexType>
class SharedLockGuard : private NonCopyable {
public:
    explicit SharedLockGuard(SharedMutexType& lock)
        : lock_(lock) {
        // 进入作用域时获取共享读锁，允许多个读线程同时进入。
        lock_.lock_shared();
    }

    ~SharedLockGuard() {
        // 离开作用域时释放共享读锁。
        lock_.unlock_shared();
    }

private:
    // 被管理的读写锁对象引用，生命周期由调用方保证。
    SharedMutexType& lock_;
};

// 读写锁的独占写守卫。
template <typename SharedMutexType>
class UniqueLockGuard : private NonCopyable {
public:
    explicit UniqueLockGuard(SharedMutexType& lock)
        : lock_(lock) {
        // 进入作用域时获取独占写锁。
        lock_.lock();
    }

    ~UniqueLockGuard() {
        // 离开作用域时释放独占写锁。
        lock_.unlock();
    }

private:
    // 被管理的读写锁对象引用，生命周期由调用方保证。
    SharedMutexType& lock_;
};

} // namespace concurrency

#endif // CONCURRENCY_LOCK_GUARD_HPP
