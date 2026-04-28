# 标准库并发接口与本项目实现对比

本文对比 C++ 标准库并发接口和本项目自实现锁的差异。

## 1. 总体对比

| 项目 | C++ 标准库接口 | 本项目实现 |
| --- | --- | --- |
| 目标 | 提供通用、跨平台、生产可用的并发抽象 | 展示锁的核心实现原理 |
| 实现方式 | 标准不规定，通常封装系统线程库和 OS 同步原语 | 直接基于 C++11 原子操作实现 |
| 互斥锁 | `std::mutex` | `concurrency::Mutex` |
| 读写锁 | C++11 无标准读写锁；C++14/17 才提供共享锁相关接口 | `concurrency::SharedMutex` |
| 等待方式 | 竞争时通常可进入内核睡眠等待 | 自旋等待，失败时 `yield()` |
| RAII | `std::lock_guard`、`std::unique_lock` | `LockGuard`、`SharedLockGuard`、`UniqueLockGuard` |
| 条件变量 | `std::condition_variable` | 暂未实现 |
| 超时锁 | `std::timed_mutex`、`std::recursive_timed_mutex` | 暂未实现 |
| 递归锁 | `std::recursive_mutex` | 暂未实现 |

## 2. Mutex 对比

| 维度 | std::mutex | concurrency::Mutex |
| --- | --- | --- |
| 底层状态 | 实现相关，常见为 pthread mutex 或系统互斥量 | `std::atomic_flag locked_` |
| 加锁方式 | 可能先用户态尝试，失败后进入内核等待 | `test_and_set()` 自旋 |
| 解锁方式 | 唤醒等待线程，具体由系统库处理 | `clear()` 释放标志 |
| CPU 消耗 | 高竞争时可睡眠，CPU 消耗较低 | 高竞争时会反复自旋，CPU 消耗较高 |
| 公平性 | 由实现决定，通常比简单自旋锁更成熟 | 不保证严格公平 |
| 适用场景 | 通用生产代码 | 短临界区、学习、面试展示 |

## 3. SharedMutex 对比

严格来说，C++11 标准库没有 `std::shared_mutex`：

- `std::shared_timed_mutex`：C++14。
- `std::shared_mutex`：C++17。

本项目在 C++11 下自己实现 `SharedMutex`。

| 维度 | 标准共享锁接口 | concurrency::SharedMutex |
| --- | --- | --- |
| 标准版本 | C++14/C++17 起 | C++11 |
| 底层状态 | 实现相关，可能封装 pthread rwlock、SRWLOCK 或自定义状态机 | `std::atomic<int> state_` |
| 读锁状态 | 实现内部维护 | `state_ > 0` 表示读者数量 |
| 写锁状态 | 实现内部维护 | `state_ == -1` 表示写线程持锁 |
| 空闲状态 | 实现内部维护 | `state_ == 0` |
| 写优先 | 实现相关 | 通过 `waiting_writers_` 做简化写优先 |
| 写线程顺序 | 实现相关 | 所有写线程通过 writer ticket 进入，阻塞式写线程 FIFO，`try_lock()` 不插队 |
| 阻塞等待 | 通常可阻塞睡眠 | 自旋 + `yield()` |
| 超时共享锁 | 标准 timed 版本支持 | 暂不支持 |

## 4. RAII 对比

| 维度 | 标准库 RAII | 本项目 RAII |
| --- | --- | --- |
| 简单作用域守卫 | `std::lock_guard` | `LockGuard` |
| 可移动锁管理 | `std::unique_lock` | 暂不支持 |
| 延迟加锁 | `std::unique_lock` 支持 | 暂不支持 |
| 提前解锁 | `std::unique_lock` 支持 | 暂不支持 |
| 条件变量配合 | `std::unique_lock` 可配合 `condition_variable` | 暂不支持 |
| 共享读锁守卫 | `std::shared_lock` 是 C++14 | `SharedLockGuard` |

本项目 RAII 守卫的重点是展示最核心的思想：构造加锁，析构解锁。

## 5. 底层机制对比

### 标准库常见路径

`std::mutex` 的常见路径：

```text
C++ std::mutex
    -> pthread_mutex_t / Windows synchronization primitive
        -> 用户态原子操作快速路径
        -> 竞争失败时进入内核等待
        -> 解锁时唤醒等待线程
```

特点：

- 无竞争时很快。
- 有竞争时可以让线程睡眠，减少 CPU 浪费。
- 实现复杂，但生产环境更可靠。

### 本项目路径

`concurrency::Mutex`：

```text
lock()
    -> atomic_flag.test_and_set()
    -> 成功：进入临界区
    -> 失败：yield 后继续重试
```

`concurrency::SharedMutex`：

```text
写锁：
state_ == 0
    -> CAS state_ 为 -1
    -> 成功后进入写临界区

读锁：
state_ >= 0 且没有等待写线程
    -> CAS state_ 为 state_ + 1
    -> 成功后进入读临界区
```

特点：

- 代码短，状态清晰，适合学习和讲解。
- 没有系统调用等待队列。
- 高竞争时性能不如成熟标准库锁。

## 6. 面试讲解角度

这个项目适合强调：

- 为什么锁需要原子操作。
- CAS 如何保证状态转换不被竞争破坏。
- acquire/release 内存序如何保证临界区可见性。
- 自旋锁和阻塞锁的区别。
- 读写锁如何允许多读单写。
- 写线程饥饿问题为什么存在。
- 为什么工程生产环境通常优先使用标准库锁。

也需要主动说明限制：

- 当前实现不是替代标准库锁的生产级方案。
- 它是为了展示并发基础、原子操作、内存序和锁状态机。
- 真正生产环境还要考虑公平性、可诊断性、超时、死锁检测、平台调度行为和性能退化。

## 7. 当前源码确认

当前项目核心实现没有调用以下标准库锁接口：

- `std::mutex`
- `std::shared_mutex`
- `std::condition_variable`
- `std::lock_guard`
- `std::unique_lock`

当前项目使用的关键底层能力：

- `std::atomic_flag`
- `std::atomic<int>`
- `compare_exchange_weak`
- `compare_exchange_strong`
- `test_and_set`
- `clear`
- `fetch_add`
- `fetch_sub`
- `std::thread`，仅用于创建演示和测试线程

## 8. Linux 执行路径对比

### 8.1 std::mutex 常见执行路径

Linux 上常见的标准库互斥锁路径可以理解为：

```text
std::mutex::lock()
    -> libstdc++/libc++ 标准库封装
    -> pthread_mutex_lock
    -> 用户态检查 mutex 内部状态
    -> 无竞争：原子操作成功，直接返回
    -> 有竞争：可能自旋，之后 futex wait
    -> Linux 内核把线程挂入等待队列
    -> 线程睡眠，不再消耗 CPU 时间片

std::mutex::unlock()
    -> pthread_mutex_unlock
    -> 用户态释放 mutex 状态
    -> 如果存在等待者，futex wake
    -> Linux 内核唤醒一个或多个等待线程
```

这个路径的重点是混合策略：

- 快路径在用户态。
- 慢路径进内核睡眠。
- 有竞争时减少 CPU 空转。

### 8.2 本项目 Mutex 执行路径

```text
concurrency::Mutex::lock()
    -> atomic_flag.test_and_set(acquire)
    -> 成功：直接进入临界区
    -> 失败：std::this_thread::yield()
    -> 再次 test_and_set

concurrency::Mutex::unlock()
    -> atomic_flag.clear(release)
```

这个路径没有 futex wait，也没有内核等待队列。

优点：

- 实现简单。
- 状态透明。
- 无竞争时很快。

缺点：

- 高竞争时大量线程重复争抢同一个缓存行。
- `yield()` 只是调度提示，不保证公平，也不保证立即让目标线程运行。
- 等待线程没有真正睡眠。

### 8.3 标准读写锁常见执行路径

标准读写锁或 pthread rwlock 常需要维护：

```text
读者数量
写者状态
等待读者队列
等待写者队列
唤醒策略
```

慢路径可能进入内核等待：

```text
读线程发现写线程活跃
    -> 进入等待队列

写线程发现读者数量不为 0
    -> 进入等待队列

最后一个读线程退出
    -> 唤醒写线程
```

### 8.4 本项目 SharedMutex 执行路径

```text
写线程:
    领取 writer ticket
    waiting_writers_++
    等待 serving ticket 轮到自己
    循环 CAS: state_ 0 -> -1
    成功后 waiting_writers_--

读线程:
    如果 waiting_writers_ > 0，暂缓进入
    如果 state_ >= 0，CAS: state_ -> state_ + 1
    如果 state_ == -1，等待
```

它把核心状态压缩成一个原子整数，所以容易讲清楚：

```text
-1 = 写锁
 0 = 空闲
 N = N 个读者
```

但它没有等待队列，所以不能做到标准库那样成熟的阻塞唤醒和公平调度。
当前版本增加了 writer ticket，能够保证阻塞式写线程之间 FIFO，但读线程和写线程之间仍采用简化写优先策略。

## 9. 什么时候用哪种实现

| 场景 | 更适合标准库锁 | 更适合本项目这种自旋锁 |
| --- | --- | --- |
| 生产业务代码 | 是 | 否 |
| 临界区可能较长 | 是 | 否 |
| 锁竞争激烈 | 是 | 否 |
| 需要超时等待 | 是 | 否 |
| 需要条件变量配合 | 是 | 否 |
| 学习原子操作 | 一般 | 是 |
| 面试讲解锁原理 | 一般 | 是 |
| 极短临界区实验 | 视情况 | 可以 |

## 10. 一句话总结

标准库锁解决的是“工程可用性”，通常结合用户态原子操作和 Linux 内核等待机制。  
本项目解决的是“原理可见性”，用 C++11 原子操作直接展示互斥锁和读写锁的状态机。
