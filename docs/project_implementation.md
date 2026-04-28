# 本项目锁实现方式

本文说明本项目当前的锁实现，包括文件结构、核心状态、接口语义和并发正确性依据。

如果需要逐个理解本项目依赖的 `test_and_set`、CAS、`fetch_add`、`fetch_sub`、acquire/release 的底层原理，请阅读 [C++11 原子操作底层原理与实现](atomic_operations_deep_dive.md)。

## 1. 设计目标

本项目目标是用 C++11 自己实现基础同步组件，用于求职项目展示。

约束：

- 不使用 `std::mutex`。
- 不使用 `std::shared_mutex`。
- 不使用 `std::condition_variable`。
- 不使用 `std::lock_guard` 或 `std::unique_lock`。
- 锁核心只使用 C++11 原子操作实现。
- `std::thread` 只用于示例和测试中的线程创建。

## 2. 文件结构

```text
include/concurrency/mutex.hpp
src/mutex.cpp
include/concurrency/shared_mutex.hpp
src/shared_mutex.cpp
include/concurrency/lock_guard.hpp
include/concurrency/noncopyable.hpp
examples/lock_demo.cpp
tests/lock_tests.cpp
```

## 3. Mutex 实现

源码：

- `include/concurrency/mutex.hpp`
- `src/mutex.cpp`

### 核心变量

```cpp
std::atomic_flag locked_;
```

变量含义：

- `false`：当前没有线程持有锁。
- `true`：当前已经有线程持有锁。

### lock()

核心逻辑：

```cpp
while (locked_.test_and_set(std::memory_order_acquire)) {
    std::this_thread::yield();
}
```

执行过程：

1. 当前线程调用 `test_and_set()`。
2. `test_and_set()` 把 `locked_` 设置为 `true`，并返回旧值。
3. 如果旧值是 `false`，说明当前线程成功获得锁。
4. 如果旧值是 `true`，说明锁已经被其他线程持有，当前线程继续自旋。
5. 自旋失败时调用 `yield()`，提示调度器让出 CPU。

### try_lock()

核心逻辑：

```cpp
return !locked_.test_and_set(std::memory_order_acquire);
```

执行过程：

- 旧值为 `false`：获取成功，返回 `true`。
- 旧值为 `true`：获取失败，返回 `false`。

### unlock()

核心逻辑：

```cpp
locked_.clear(std::memory_order_release);
```

执行过程：

- 把锁状态恢复为 `false`。
- 使用 release 内存序，保证临界区写入对后续成功加锁线程可见。

## 4. SharedMutex 实现

源码：

- `include/concurrency/shared_mutex.hpp`
- `src/shared_mutex.cpp`

### 核心变量

```cpp
std::atomic<int> state_;
std::atomic<int> waiting_writers_;
std::atomic<unsigned long long> next_writer_ticket_;
std::atomic<unsigned long long> serving_writer_ticket_;
```

`state_` 含义：

- `-1`：当前有一个写线程持有独占锁。
- `0`：当前没有线程持锁。
- `>0`：当前持有共享读锁的线程数量。

`waiting_writers_` 含义：

- 当前正在等待写锁的线程数量。
- 读线程会检查它，避免在写线程等待时继续插队。

`next_writer_ticket_` 含义：

- 下一个阻塞式写线程要领取的 ticket 编号。
- 每次 `lock()` 调用都会通过 `fetch_add(1)` 获取唯一编号。

`serving_writer_ticket_` 含义：

- 当前轮到哪个写线程尝试获取写锁。
- 只有 `my_ticket == serving_writer_ticket_` 的写线程才允许 CAS `state_`。

### lock()

写线程获取独占锁。

核心逻辑：

```cpp
const unsigned long long my_ticket =
    next_writer_ticket_.fetch_add(1, std::memory_order_acq_rel);

waiting_writers_.fetch_add(1, std::memory_order_acq_rel);

for (;;) {
    if (serving_writer_ticket_.load(std::memory_order_acquire) != my_ticket) {
        std::this_thread::yield();
        continue;
    }

    int expected_state = 0;
    if (state_.compare_exchange_weak(expected_state,
                                     -1,
                                     std::memory_order_acquire,
                                     std::memory_order_relaxed)) {
        waiting_writers_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    std::this_thread::yield();
}
```

执行过程：

1. 写线程先从 `next_writer_ticket_` 领取自己的 `my_ticket`。
2. 写线程增加 `waiting_writers_`，通知新读线程暂缓进入。
3. 只有当 `my_ticket == serving_writer_ticket_` 时，当前写线程才有资格竞争写锁。
4. 只有当 `state_ == 0` 时，说明没有读线程也没有写线程。
5. 写线程用 CAS 把 `state_` 从 `0` 改成 `-1`。
6. 修改成功表示获得写锁。
7. 修改失败表示仍有读线程或 CAS 虚假失败，继续自旋等待。

### try_lock()

写线程非阻塞尝试获取独占锁。

核心逻辑：

```cpp
int expected_state = 0;
const unsigned long long serving_ticket =
    serving_writer_ticket_.load(std::memory_order_acquire);
unsigned long long expected_next_ticket = serving_ticket;

if (!next_writer_ticket_.compare_exchange_strong(expected_next_ticket,
                                                 serving_ticket + 1,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
    return false;
}

const bool locked = state_.compare_exchange_strong(expected_state,
                                                   -1,
                                                   std::memory_order_acquire,
                                                   std::memory_order_relaxed);

if (!locked) {
    serving_writer_ticket_.fetch_add(1, std::memory_order_acq_rel);
}

return locked;
```

执行过程：

- `state_ == 0`：改成 `-1`，获取成功。
- `state_ != 0`：不等待，直接失败。
- `try_lock()` 也必须先预留 writer ticket。
- 如果已经有写线程排队，`try_lock()` 预留 ticket 失败，直接返回失败，避免插队。
- 如果预留 ticket 后没有拿到锁，需要推进 `serving_writer_ticket_` 跳过这个未使用 ticket。

### unlock()

写线程释放独占锁。

核心逻辑：

```cpp
state_.store(0, std::memory_order_release);
serving_writer_ticket_.fetch_add(1, std::memory_order_acq_rel);
```

执行过程：

- 当前写线程把状态从 `-1` 恢复为 `0`。
- release 内存序保证写线程临界区的数据修改被后续读/写线程看见。
- 推进 `serving_writer_ticket_`，让下一个写线程继续。

### lock_shared()

读线程获取共享锁。

核心逻辑：

```cpp
for (;;) {
    if (waiting_writers_.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
        continue;
    }

    int current_state = state_.load(std::memory_order_acquire);

    if (current_state < 0) {
        std::this_thread::yield();
        continue;
    }

    if (state_.compare_exchange_weak(current_state,
                                     current_state + 1,
                                     std::memory_order_acquire,
                                     std::memory_order_relaxed)) {
        return;
    }

    std::this_thread::yield();
}
```

执行过程：

1. 如果有写线程等待，读线程暂缓进入。
2. 读取 `state_`。
3. 如果 `state_ < 0`，说明写线程正在持锁，读线程等待。
4. 如果 `state_ >= 0`，说明当前没有写线程持锁。
5. 读线程用 CAS 把读者数量加 1。
6. CAS 成功表示获得共享读锁。

### try_lock_shared()

读线程非阻塞尝试获取共享锁。

执行过程：

- 如果有写线程等待，直接返回失败。
- 如果有写线程持锁，直接返回失败。
- 如果当前只有读线程或无人持锁，则尝试把 `state_` 加 1。

### unlock_shared()

读线程释放共享锁。

核心逻辑：

```cpp
state_.fetch_sub(1, std::memory_order_release);
```

执行过程：

- 当前读线程把读者数量减 1。
- 最后一个读线程退出后，`state_` 变回 `0`，写线程才可能成功获取锁。

## 5. RAII 守卫实现

源码：

- `include/concurrency/lock_guard.hpp`

### LockGuard

用于普通互斥锁：

```cpp
template <typename LockType>
class LockGuard {
public:
    explicit LockGuard(LockType& lock) : lock_(lock) {
        lock_.lock();
    }

    ~LockGuard() {
        lock_.unlock();
    }

private:
    LockType& lock_;
};
```

语义：

- 构造时加锁。
- 析构时解锁。
- 防止忘记解锁。
- 防止异常或提前 return 导致锁泄漏。

### SharedLockGuard

用于共享读锁：

- 构造时调用 `lock_shared()`。
- 析构时调用 `unlock_shared()`。

### UniqueLockGuard

用于读写锁的独占写锁：

- 构造时调用 `lock()`。
- 析构时调用 `unlock()`。

## 6. 正确性依据

### 互斥锁正确性

`std::atomic_flag::test_and_set()` 是原子读改写操作。

同一时刻只有一个线程能观察到旧值为 `false`，因此只有一个线程能进入临界区。

### 读写锁正确性

`state_` 是唯一的锁状态来源：

- 写锁只能从 `0` CAS 到 `-1`。
- 读锁只能在 `state_ >= 0` 时 CAS 到 `state_ + 1`。
- 写锁存在时，`state_ == -1`，读线程无法加读者计数。
- 读线程存在时，`state_ > 0`，写线程无法把状态改成 `-1`。

因此：

- 写线程和写线程互斥。
- 写线程和读线程互斥。
- 多个读线程可以并发。

### 内存可见性

本项目采用典型锁内存序：

- 加锁成功使用 `memory_order_acquire`。
- 解锁使用 `memory_order_release`。

这保证一个线程在临界区内的写入，对之后成功获得同一把锁的线程可见。

## 7. 已知限制

当前实现是自旋锁风格，有以下限制：

- 竞争严重或临界区很长时会浪费 CPU。
- 没有进入内核等待队列，不能像系统互斥锁一样睡眠等待。
- 公平性仍不是完整读写线程统一 FIFO；当前只保证阻塞式写线程 FIFO，并通过 `waiting_writers_` 降低写线程饥饿概率。
- 没有记录 owner，错误 unlock 不会被诊断。
- 不支持超时接口。
- 不支持递归锁。

这些限制本身也适合作为面试讨论点：可以解释从自旋锁升级到阻塞锁、从简单读写锁升级到公平读写锁需要解决哪些问题。

## 8. 从零实现 Mutex 的完整思路

如果自己从零实现互斥锁，可以按以下步骤推导。

### 8.1 先定义状态

互斥锁只有两种状态：

```text
0 / false: 未加锁
1 / true : 已加锁
```

本项目使用：

```cpp
std::atomic_flag locked_;
```

`atomic_flag` 是 C++11 保证无锁的原子标志类型，适合教学实现自旋锁。

### 8.2 再定义状态转换

互斥锁只有两个关键转换：

```text
加锁: false -> true
解锁: true  -> false
```

加锁转换必须是原子的。不能先判断再赋值，因为两个线程可能同时看到 `false`。

### 8.3 用 test_and_set 实现加锁

`test_and_set()` 做两件事：

```text
读取旧值
把值设置为 true
```

这两件事作为一个原子操作完成。

所以：

```cpp
while (locked_.test_and_set(std::memory_order_acquire)) {
    std::this_thread::yield();
}
```

含义是：

- 旧值为 `false`：说明之前没人持锁，当前线程成功进入。
- 旧值为 `true`：说明锁已经被持有，当前线程继续等待。

### 8.4 用 clear 实现解锁

```cpp
locked_.clear(std::memory_order_release);
```

这一步把锁恢复为空闲状态。

release 内存序很关键。它保证临界区内对共享数据的写入不会被重排到解锁之后。

### 8.5 为什么 try_lock 可以一行实现

`try_lock()` 不等待，只尝试一次：

```cpp
bool Mutex::try_lock() {
    return !locked_.test_and_set(std::memory_order_acquire);
}
```

如果返回 `true`，当前线程已经持锁。  
如果返回 `false`，锁仍由其他线程持有。

### 8.6 Mutex 状态图

```text
        try_lock/lock 成功
   +------------------------+
   |                        v
+--------+              +--------+
| 空闲   |              | 已占用 |
| false  |              | true   |
+--------+              +--------+
   ^                        |
   |                        |
   +------------------------+
            unlock
```

## 9. 从零实现 SharedMutex 的完整思路

读写锁比互斥锁复杂，因为它有三类状态：

```text
无人持锁
一个写线程持锁
多个读线程持锁
```

### 9.1 朴素设计

直观上可以定义三个变量：

```text
active_readers: 当前读线程数量
writer_active : 是否有写线程持锁
waiting_writers: 等待写锁的线程数量
```

但如果这几个变量不是在同一个锁保护下修改，就会出现状态不一致。

本项目不允许用 `std::mutex` 保护这些变量，所以选择把最关键的读写状态压缩到一个原子整数里。

### 9.2 压缩成一个 state_

本项目定义：

```text
state_ == -1: 写线程持锁
state_ == 0 : 无人持锁
state_ > 0  : 读线程数量
```

这个设计的好处是：

- 读者数量和写锁状态不会分裂。
- 所有读写状态转换都能通过一个 CAS 保护。
- 状态含义适合画图和面试讲解。

### 9.3 写锁 lock() 的状态转换

写线程只允许在无人持锁时进入：

```text
0 -> -1
```

实现：

```cpp
int expected_state = 0;
state_.compare_exchange_weak(expected_state, -1);
```

CAS 成功表示：

- 之前确实无人持锁。
- 当前线程已经把状态改成写锁状态。
- 其他线程无法同时完成同样的转换。

CAS 失败表示：

- `state_ > 0`：有读线程。
- `state_ == -1`：有写线程。
- 当前线程必须等待。

### 9.4 读锁 lock_shared() 的状态转换

读线程允许在没有写线程持锁时进入：

```text
0 -> 1
1 -> 2
2 -> 3
...
```

不允许：

```text
-1 -> 0
-1 -> 1
```

实现思路：

```cpp
int current_state = state_.load();
if (current_state >= 0) {
    state_.compare_exchange_weak(current_state, current_state + 1);
}
```

CAS 的意义是：

- 线程先读取当前读者数量。
- 尝试把数量加 1。
- 如果期间有其他读线程也修改了数量，CAS 失败并重新读取。
- 如果期间写线程抢到了锁，状态变成 `-1`，读线程不能进入。

### 9.5 读锁 unlock_shared() 的状态转换

读线程退出时只需要减少读者数量：

```text
3 -> 2
2 -> 1
1 -> 0
```

实现：

```cpp
state_.fetch_sub(1, std::memory_order_release);
```

最后一个读线程退出后，`state_` 变成 `0`，等待中的写线程才可能 CAS 成功。

### 9.6 写优先 waiting_writers_

如果没有 `waiting_writers_`，读线程可能源源不断进入：

```text
R1 已进入
W 开始等待
R2 进入
R3 进入
R4 进入
W 一直等不到 state_ == 0
```

所以写线程等待前先执行：

```cpp
const unsigned long long my_ticket =
    next_writer_ticket_.fetch_add(1);
waiting_writers_.fetch_add(1);
```

读线程进入前检查：

```cpp
if (waiting_writers_.load() > 0) {
    yield();
    continue;
}
```

效果：

- 已经进入的读线程可以正常退出。
- 新读线程不再插队。
- 写线程更容易等到 `state_ == 0`。
- 多个写线程之间按 ticket 顺序进入，避免写线程之间无序抢锁。

这是一种“写线程 FIFO + 简化写优先”的策略。它比普通 CAS 抢锁更可控，但仍不是完整的读写线程统一 FIFO 队列。

### 9.7 SharedMutex 状态图

```text
                  写锁 lock 成功
             +----------------------+
             |                      v
+------------+-------------+    +----------+
| 无人持锁 state_ == 0     |    | 写持锁   |
+------------+-------------+    | state=-1 |
             ^                  +----------+
             |                      |
             |                      |
             +----------------------+
                    写锁 unlock


+------------+       读锁 lock_shared       +------------+
| state_ = 0 | ---------------------------> | state_ = 1 |
+------------+                              +------------+
                                                   |
                                                   | 更多读线程进入
                                                   v
                                             +------------+
                                             | state_ = N |
                                             +------------+
                                                   |
                                                   | 读线程退出
                                                   v
                                             +------------+
                                             | state_ = 0 |
                                             +------------+
```

## 10. 和 Linux 内核/系统锁的关系

本项目实现的是用户态自旋锁，不进入 Linux 内核等待队列。

等待路径：

```text
竞争失败
    -> 当前线程仍在用户态运行
    -> 调用 yield 提示调度器让出 CPU
    -> 之后继续尝试原子操作
```

标准库 `std::mutex` 在 Linux 上通常更接近：

```text
竞争失败
    -> 用户态短暂尝试
    -> 仍失败则 futex wait
    -> 线程进入内核等待队列并睡眠
    -> 解锁线程 futex wake 唤醒
```

因此本项目更适合短临界区：

- 加锁释放都是用户态原子操作。
- 无系统调用快路径。
- 逻辑透明，适合讲解。

但它不适合长时间持锁：

- 等待线程会不断被调度运行。
- CPU 可能浪费在重试上。
- 没有内核等待队列的唤醒控制。
