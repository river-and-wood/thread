#ifndef CONCURRENCY_NONCOPYABLE_HPP
#define CONCURRENCY_NONCOPYABLE_HPP

namespace concurrency {

// 禁止拷贝的基类。
// 锁对象通常不应该被拷贝，否则多个对象会错误地表示同一个同步状态。
class NonCopyable {
protected:
    NonCopyable() {}
    ~NonCopyable() {}

private:
    NonCopyable(const NonCopyable&);
    NonCopyable& operator=(const NonCopyable&);
};

} // namespace concurrency

#endif // CONCURRENCY_NONCOPYABLE_HPP
