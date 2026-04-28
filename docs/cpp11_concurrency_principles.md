# C++11 并发接口与底层原理

本文说明 C++11 标准库提供的多线程、锁、条件同步和原子操作接口，以及它们通常在系统层面的实现方式。

注意：C++ 标准只规定接口语义，不规定具体底层实现。下面的“底层实现”是主流实现的常见方式，例如 Linux + libstdc++/libc++、Windows STL，而不是标准强制要求。

如果需要更偏 Linux 内核、futex、调度器、缓存一致性和自旋锁/阻塞锁差异的分析，请继续阅读 [Linux/操作系统视角下的锁底层原理](linux_os_locking_deep_dive.md)。

如果需要深入到本项目实际用到的每个原子操作，请阅读 [C++11 原子操作底层原理与实现](atomic_operations_deep_dive.md)。

## 1. C++11 多线程基础接口

### std::thread

`std::thread` 用来创建和管理一个执行线程。

常用接口：

```cpp
std::thread t(func);
t.join();
t.detach();
t.joinable();
std::this_thread::yield();
std::this_thread::sleep_for(duration);
```

核心语义：

- 构造 `std::thread` 后，新线程开始执行传入的函数。
- `join()` 等待线程结束，并回收线程资源。
- `detach()` 让线程脱离当前对象独立运行。
- `yield()` 提示调度器当前线程愿意让出 CPU。

常见底层实现：

- POSIX/Linux：通常封装 `pthread_create`、`pthread_join`、`pthread_detach`。
- Windows：通常封装 Windows 线程 API，例如 `CreateThread` 或运行库线程接口。
- 线程调度由操作系统内核负责，C++ 标准库只提供跨平台封装。

## 2. C++11 锁接口

### std::mutex

`std::mutex` 是不可递归互斥锁，同一时间只允许一个线程进入临界区。

常用接口：

```cpp
std::mutex m;
m.lock();
bool ok = m.try_lock();
m.unlock();
```

核心语义：

- `lock()` 阻塞直到获取锁。
- `try_lock()` 立即尝试获取锁，失败时返回 `false`。
- `unlock()` 释放锁。
- 同一个线程重复 `lock()` 一个已经持有的 `std::mutex` 是未定义行为。

常见底层实现：

- Linux 上通常封装 `pthread_mutex_t`。
- `pthread_mutex_t` 的实现一般包含用户态快速路径和内核态慢路径。
- 无竞争时，通常只需要用户态原子 CAS 或交换指令。
- 有竞争时，线程可能通过 futex 等机制进入内核等待队列，避免一直消耗 CPU。

### std::recursive_mutex

`std::recursive_mutex` 是递归互斥锁，同一线程可以多次加锁。

常用接口：

```cpp
std::recursive_mutex m;
m.lock();
m.try_lock();
m.unlock();
```

核心语义：

- 内部需要记录“当前持有锁的线程 ID”和“递归加锁次数”。
- 同一线程多次 `lock()` 会增加计数。
- 必须调用相同次数的 `unlock()`，锁才真正释放。

常见底层实现：

- POSIX 平台常封装递归属性的 `pthread_mutex_t`。
- 内部比普通 mutex 多维护 owner 和 recursion count，因此开销通常更大。

### std::timed_mutex

`std::timed_mutex` 是支持超时等待的互斥锁。

常用接口：

```cpp
std::timed_mutex m;
m.lock();
m.try_lock();
m.try_lock_for(duration);
m.try_lock_until(time_point);
m.unlock();
```

核心语义：

- `try_lock_for()` 在指定时间内尝试获取锁。
- `try_lock_until()` 在指定时间点之前尝试获取锁。

常见底层实现：

- 底层需要结合互斥状态、等待队列和系统时钟。
- POSIX 平台可能映射到带超时能力的 pthread mutex 或条件等待机制。
- 等待失败可能来自锁竞争，也可能来自超时。

### std::recursive_timed_mutex

`std::recursive_timed_mutex` 是递归锁和超时锁的组合。

核心语义：

- 支持同一线程重复加锁。
- 支持限时等待。
- 内部需要同时维护 owner、递归计数和超时等待。

## 3. C++11 RAII 锁管理器

### std::lock_guard

`std::lock_guard` 是最简单的 RAII 锁守卫。

```cpp
{
    std::lock_guard<std::mutex> guard(m);
    // 临界区
}
```

核心语义：

- 构造时调用 `lock()`。
- 析构时调用 `unlock()`。
- 不能手动提前解锁。
- 适合简单作用域临界区。

### std::unique_lock

`std::unique_lock` 是更灵活的 RAII 锁管理器。

```cpp
std::unique_lock<std::mutex> lock(m);
lock.unlock();
lock.lock();
```

核心语义：

- 可以延迟加锁。
- 可以提前解锁。
- 可以转移所有权。
- 是 `std::condition_variable` 的常用搭配类型。

底层特点：

- 本身不是锁，只是管理一个锁对象的所有权状态。
- 通常保存锁对象指针和一个 owns-lock 布尔状态。

## 4. C++11 条件同步

### std::condition_variable

`std::condition_variable` 用于线程等待某个条件成立。

常用形式：

```cpp
std::mutex m;
std::condition_variable cv;
bool ready = false;

std::unique_lock<std::mutex> lock(m);
cv.wait(lock, [&] { return ready; });
```

核心语义：

- 等待时会原子地释放互斥锁并阻塞当前线程。
- 被唤醒后会重新获取互斥锁。
- 可能发生虚假唤醒，所以必须配合条件谓词使用。

常见底层实现：

- POSIX/Linux：通常封装 `pthread_cond_t`。
- 内核或运行库维护等待队列。
- `notify_one()` 唤醒一个等待线程。
- `notify_all()` 唤醒所有等待线程。

## 5. C++11 原子操作

### std::atomic

`std::atomic<T>` 提供无数据竞争的原子读写和读改写操作。

常用接口：

```cpp
std::atomic<int> value(0);
value.load();
value.store(1);
value.fetch_add(1);
value.compare_exchange_weak(expected, desired);
value.compare_exchange_strong(expected, desired);
```

核心语义：

- 单个原子对象上的操作不会被其他线程观察到中间状态。
- 可用于实现自旋锁、引用计数、无锁队列等基础组件。
- 是否真正 lock-free 取决于类型、平台和编译器。

常见底层实现：

- x86/x64：常映射到 `lock cmpxchg`、`xchg`、`lock xadd` 等 CPU 指令。
- ARM：常映射到 load-exclusive/store-exclusive 或新的原子指令。
- 如果目标类型无法用硬件原子指令实现，标准库可能退化到内部锁。

### std::atomic_flag

`std::atomic_flag` 是 C++11 保证无锁的最小原子标志类型。

常用接口：

```cpp
std::atomic_flag flag = ATOMIC_FLAG_INIT;
bool old = flag.test_and_set();
flag.clear();
```

典型用途：

- 实现自旋锁。
- `test_and_set()` 把标志设为 true，并返回旧值。
- `clear()` 把标志恢复为 false。

## 6. 内存序

C++11 原子操作支持内存序参数，决定编译器和 CPU 如何约束指令重排。

常见内存序：

- `memory_order_relaxed`：只保证原子性，不建立同步关系。
- `memory_order_acquire`：获取语义，后续读写不能重排到它之前。
- `memory_order_release`：释放语义，之前读写不能重排到它之后。
- `memory_order_acq_rel`：同时具备 acquire 和 release。
- `memory_order_seq_cst`：最强顺序一致性，默认内存序，约束最强，成本可能更高。

锁的常见内存序模型：

- 加锁成功使用 acquire。
- 解锁使用 release。
- 这样可以保证一个线程在临界区内写入的数据，对下一个成功加锁的线程可见。

## 7. C++11 没有的共享锁接口

严格来说：

- `std::shared_timed_mutex` 是 C++14 引入。
- `std::shared_mutex` 是 C++17 引入。

因此如果项目要求纯 C++11，又想要读写锁，需要自己实现，或使用平台 API / 第三方库。本项目选择自己基于 C++11 原子操作实现一个自旋读写锁。
