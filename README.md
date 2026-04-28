# C++11 Concurrency Locks

这是一个面向求职展示的 C++11 多线程项目骨架，目标是从零实现常见同步工具，并配套示例和测试。

## 当前模块

- `Mutex`：基于 `std::atomic_flag` 的自旋互斥锁。
- `SharedMutex`：基于 `std::atomic<int>` 状态机的自旋读写锁。
- `LockGuard`：互斥锁 RAII 守卫，构造时加锁，析构时解锁。
- `SharedLockGuard`：共享读锁 RAII 守卫。
- `UniqueLockGuard`：独占写锁 RAII 守卫。

锁的核心实现不封装 `std::mutex`、`std::shared_mutex`、`std::condition_variable` 这类现成锁接口，只使用 C++11 原子操作作为基础。

## 已实现接口

```cpp
// Mutex
void lock();
bool try_lock();
void unlock();

// SharedMutex
void lock();
bool try_lock();
void unlock();
void lock_shared();
bool try_lock_shared();
void unlock_shared();
```

## 目录结构

```text
.
├── CMakeLists.txt
├── README.md
├── include/concurrency
│   ├── lock_guard.hpp
│   ├── mutex.hpp
│   ├── noncopyable.hpp
│   └── shared_mutex.hpp
├── src
│   ├── mutex.cpp
│   └── shared_mutex.cpp
├── examples
│   └── lock_demo.cpp
└── tests
    └── lock_tests.cpp
```

## 编译运行

```bash
cmake -S . -B build
cmake --build build
./build/lock_demo
ctest --test-dir build --output-on-failure
```

## 文档

- [C++11 并发接口与底层原理](docs/cpp11_concurrency_principles.md)
- [Linux/操作系统视角下的锁底层原理](docs/linux_os_locking_deep_dive.md)
- [C++11 原子操作底层原理与实现](docs/atomic_operations_deep_dive.md)
- [本项目锁实现方式](docs/project_implementation.md)
- [标准库并发接口与本项目实现对比](docs/comparison.md)

## 后续可扩展方向

- 给 `Mutex` 增加 `try_lock()`。
- 给 `SharedMutex` 增加更严格的公平调度策略。
- 实现 `ThreadPool`，展示任务队列、条件变量和优雅退出。
- 加入性能压测，对比 `std::mutex`、`std::shared_timed_mutex`。
