# C++11 原子操作底层原理与实现

本文专门解释本项目依赖的 C++11 原子操作，包括它们解决的问题、C++ 接口语义、可能对应的 CPU 指令、缓存一致性行为、内存序含义，以及它们如何支撑本项目的互斥锁和读写锁。

本项目锁核心依赖的原子能力：

- `std::atomic_flag`
- `std::atomic<int>`
- `test_and_set`
- `clear`
- `compare_exchange_weak`
- `compare_exchange_strong`
- `fetch_add`
- `fetch_sub`
- `load`
- `store`
- `memory_order_acquire`
- `memory_order_release`
- `memory_order_acq_rel`
- `memory_order_relaxed`

## 1. 原子操作到底是什么

原子操作的核心含义是：一个操作在多线程观察下不可被拆开。

普通自增：

```cpp
++value;
```

在机器层面通常不是一步，而是类似：

```text
load  value -> register
add   register, 1
store register -> value
```

如果两个线程同时执行，可能出现丢失更新：

```text
value 初始为 0

线程 A load 得到 0
线程 B load 得到 0
线程 A add 得到 1
线程 B add 得到 1
线程 A store 1
线程 B store 1

最终 value == 1，但实际执行了两次自增
```

原子自增会把“读旧值、计算新值、写回新值”变成其他线程无法插入破坏的读改写操作。

## 2. 原子操作和锁的关系

锁的本质是一个共享状态变量。

互斥锁状态：

```text
false: 没有线程持锁
true : 已经有线程持锁
```

加锁需要做：

```text
如果状态是 false，就把它改成 true
```

这个“如果是 false 就改成 true”必须是原子的。否则两个线程可能同时看到 false，然后都进入临界区。

因此锁不是凭空实现的。锁依赖更底层的硬件原子能力。

层次关系可以理解为：

```text
CPU 原子指令
    -> C++11 std::atomic
        -> 自旋锁 / 读写锁 / 无锁数据结构
            -> 更高层并发组件
```

## 3. std::atomic_flag

`std::atomic_flag` 是 C++11 中最小的原子布尔标志类型。

本项目使用它实现 `Mutex`：

```cpp
std::atomic_flag locked_;
```

语义：

- `false` 表示锁空闲。
- `true` 表示锁被占用。

### 3.1 test_and_set

接口：

```cpp
bool old = locked_.test_and_set(std::memory_order_acquire);
```

语义：

```text
old = locked_
locked_ = true
return old
```

关键点是：读取旧值和写入 true 是一个原子读改写操作。

本项目加锁：

```cpp
while (locked_.test_and_set(std::memory_order_acquire)) {
    std::this_thread::yield();
}
```

执行过程：

```text
线程 A test_and_set，旧值 false，写成 true，返回 false，A 获得锁
线程 B test_and_set，旧值 true，继续写 true，返回 true，B 等待
线程 C test_and_set，旧值 true，继续写 true，返回 true，C 等待
```

同一时刻只有一个线程能看到旧值为 false，因此只有一个线程能进入临界区。

### 3.2 clear

接口：

```cpp
locked_.clear(std::memory_order_release);
```

语义：

```text
locked_ = false
```

这一步释放锁。之后其他线程执行 `test_and_set()` 时，才可能看到旧值为 false。

### 3.3 atomic_flag 的底层实现

C++ 标准保证 `std::atomic_flag` 是 lock-free 的原子标志。主流平台上它通常映射为一个字节或一个机器字大小的原子变量。

x86/x64 上，`test_and_set` 可能使用类似：

```text
xchg byte ptr [locked_], 1
```

或等价的带锁语义的原子交换指令。

ARM 上，可能使用独占加载/独占存储循环：

```text
LDAXR 读取 locked_
STLXR 尝试写入 true
如果失败则重试
```

具体指令由编译器、架构和优化级别决定，但语义必须满足 C++ 标准对原子读改写的要求。

## 4. std::atomic<int>

本项目使用 `std::atomic<int>` 实现读写锁：

```cpp
std::atomic<int> state_;
std::atomic<unsigned long long> next_ticket_;
std::atomic<unsigned long long> serving_ticket_;
```

`state_` 表示读写锁主状态：

```text
state_ == -1: 写线程持锁
state_ == 0 : 空闲
state_ > 0  : 读线程数量
```

`next_ticket_` 和 `serving_ticket_` 组成读写统一 FIFO 队列。读请求和写请求都从 `next_ticket_` 领取编号，只有轮到 `serving_ticket_` 时才能尝试进入。

### 4.1 load

接口：

```cpp
int value = state_.load(std::memory_order_acquire);
```

语义：

```text
原子读取 state_ 当前值
```

普通 `int` 读取在 C++ 数据竞争场景下是未定义行为。`atomic<int>::load()` 保证多个线程同时读写同一个原子对象时不会形成数据竞争。

在读写锁中，读线程通过 `load()` 判断：

```text
state_ < 0  : 写线程持锁，不能进入
state_ >= 0 : 没有写线程持锁，可以尝试增加读者数量
```

### 4.2 store

接口：

```cpp
state_.store(0, std::memory_order_release);
```

语义：

```text
原子写入 state_ = 0
```

在本项目中，写线程释放锁时执行：

```cpp
state_.store(0, std::memory_order_release);
```

含义是把状态从 `-1` 恢复为空闲。

### 4.3 fetch_add

接口：

```cpp
unsigned long long old = next_ticket_.fetch_add(1, std::memory_order_acq_rel);
```

语义：

```text
old = next_ticket_
next_ticket_ = next_ticket_ + 1
return old
```

读取旧值和加 1 写回是一个原子读改写操作。

本项目读线程和写线程排队前：

```cpp
const unsigned long long my_ticket =
    next_ticket_.fetch_add(1, std::memory_order_acq_rel);
```

作用：

- 给每个读/写请求分配唯一 ticket。
- 让所有请求按到达顺序排队。
- 防止后来的读线程越过已经排队的写线程。

### 4.4 fetch_sub

接口：

```cpp
state_.fetch_sub(1, std::memory_order_release);
```

语义：

```text
old = state_
state_ = state_ - 1
return old
```

本项目读线程释放共享锁时：

```cpp
state_.fetch_sub(1, std::memory_order_release);
```

如果原来 `state_ == 1`，执行后变成 `0`，表示最后一个读线程退出，写线程可以尝试进入。

## 5. CAS：compare_exchange

CAS 是 compare-and-swap，也叫 compare-and-exchange。

它是实现无锁状态机最重要的原子操作。

语义：

```text
if (*address == expected) {
    *address = desired;
    return true;
} else {
    expected = *address;
    return false;
}
```

注意：C++ 的 `compare_exchange` 失败时会把当前真实值写回 `expected`。

## 6. compare_exchange_strong

接口：

```cpp
int expected = 0;
bool ok = state_.compare_exchange_strong(expected,
                                         -1,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed);
```

本项目 `SharedMutex::try_lock()` 使用它尝试把 `state_` 从空闲状态改成写锁状态。当前版本还会先通过 `next_ticket_` 预留统一 ticket，避免 `try_lock()` 插队到已经排队的读/写请求前面：

```cpp
int expected_state = 0;
const unsigned long long serving_ticket =
    serving_ticket_.load(std::memory_order_acquire);
unsigned long long expected_next_ticket = serving_ticket;

if (!next_ticket_.compare_exchange_strong(expected_next_ticket,
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
    serving_ticket_.fetch_add(1, std::memory_order_acq_rel);
}

return locked;
```

含义：

```text
如果没有写线程排队，先预留当前 serving ticket
如果 state_ 当前为 0，就改成 -1，表示写线程获得锁
如果 state_ 当前不是 0，不修改 state_，并跳过刚才预留但未使用的 ticket
```

为什么适合 `try_lock()`：

- `try_lock()` 只尝试一次。
- `compare_exchange_strong` 不应该因为“虚假失败”而失败。
- 如果失败，基本表示状态确实不是 expected。

## 7. compare_exchange_weak

接口：

```cpp
state_.compare_exchange_weak(expected, desired, success_order, failure_order);
```

`weak` 版本允许虚假失败。

虚假失败指：

```text
state_ 明明等于 expected，但 compare_exchange_weak 仍然返回 false
```

为什么允许这种语义：

- 某些 CPU 架构的原子实现天然可能失败，例如 ARM 的 exclusive store。
- `weak` 版本更容易映射到底层指令。
- 在循环里使用时，虚假失败没有问题，因为下一轮会重试。

本项目阻塞式加锁使用 `weak`：

```cpp
for (;;) {
    int expected_state = 0;
    if (state_.compare_exchange_weak(expected_state,
                                     -1,
                                     std::memory_order_acquire,
                                     std::memory_order_relaxed)) {
        return;
    }
    std::this_thread::yield();
}
```

为什么可以用 `weak`：

- 这段代码本来就处在无限重试循环中。
- CAS 失败后继续尝试即可。
- 即使有虚假失败，也不会破坏正确性，只可能多循环一次。

## 8. CAS 在读写锁中的状态转换

本项目读写锁靠 CAS 保证状态转换正确。

### 8.1 写锁转换

```text
0 -> -1
```

只有无人持锁时写线程才能进入。

```cpp
int expected_state = 0;
state_.compare_exchange_weak(expected_state, -1);
```

多写线程竞争：

```text
state_ 初始为 0

线程 W1 CAS 0 -> -1 成功
线程 W2 CAS 0 -> -1 失败，因为 state_ 已经是 -1
线程 W3 CAS 0 -> -1 失败
```

结果：只有 W1 获得写锁。

### 8.2 读锁转换

```text
N -> N + 1，前提是 N >= 0
```

读线程进入：

```cpp
int current_state = state_.load(std::memory_order_acquire);

if (current_state >= 0) {
    state_.compare_exchange_weak(current_state, current_state + 1);
}
```

多读线程竞争：

```text
state_ 初始为 0

线程 R1 load 0
线程 R2 load 0
线程 R1 CAS 0 -> 1 成功
线程 R2 CAS 0 -> 1 失败，因为 state_ 已经是 1
线程 R2 重新读取 1
线程 R2 CAS 1 -> 2 成功
```

结果：两个读线程都能进入，最终 `state_ == 2`。

### 8.3 读写竞争

```text
state_ 初始为 0

线程 W load expected 0，准备 CAS 0 -> -1
线程 R load 0，准备 CAS 0 -> 1
```

可能结果一：

```text
W CAS 成功，state_ = -1
R CAS 失败，重新读取到 -1，不能进入
```

可能结果二：

```text
R CAS 成功，state_ = 1
W CAS 失败，重新读取到 1，等待读线程退出
```

无论哪种结果，都不会出现读线程和写线程同时进入。

## 9. C++ 原子操作到底怎么变成机器指令

C++ 源码：

```cpp
state_.compare_exchange_weak(expected, desired,
                             std::memory_order_acquire,
                             std::memory_order_relaxed);
```

编译器会根据目标架构生成对应机器码。

### 9.1 x86/x64

x86/x64 有较强的内存模型，很多 acquire/release 约束天然满足。

CAS 常见对应：

```text
lock cmpxchg [memory], register
```

含义：

```text
比较 accumulator 和内存值
如果相等，把 register 写到内存
如果不相等，把内存值读回 accumulator
整个过程对其他核心表现为原子
```

`fetch_add` 常见对应：

```text
lock xadd [memory], register
```

`test_and_set` 可能对应：

```text
xchg [memory], register
```

`store release` 在 x86 上很多时候就是普通 store，因为 x86 已经保证 store-store 顺序。但如果使用更强的 `seq_cst`，编译器可能生成更强约束的指令或屏障。

### 9.2 ARM/AArch64

ARM 内存模型比 x86 更弱，需要更明确的 acquire/release 指令。

CAS 可能对应两种方式。

旧式 exclusive load/store 循环：

```text
LDAXR  old, [addr]      ; acquire load exclusive
CMP    old, expected
B.NE   fail
STLXR  status, desired, [addr] ; release store exclusive
CBNZ   status, retry
```

较新 AArch64 LSE 原子指令可能使用：

```text
CASA / CASAL
LDADD / LDADDA / LDADDL / LDADDAL
```

其中指令后缀通常表达 acquire、release 或 acquire-release 语义。

### 9.3 为什么同一份 C++ 代码能跨平台

C++11 原子库提供的是语义：

```text
这个操作必须原子
这个操作必须具备指定内存序
```

编译器负责把语义映射到不同 CPU 的指令：

```text
C++ atomic
    -> 编译器内建原子操作
        -> x86 lock 指令 / ARM exclusive 指令 / RISC-V amo 指令
            -> CPU 缓存一致性协议
```

如果某种类型在目标平台不能直接用硬件原子实现，`std::atomic<T>` 可能不是 lock-free，标准库可能用内部锁辅助。但本项目使用的 `atomic_flag` 保证 lock-free；常见平台上 `atomic<int>` 也通常是 lock-free。

## 10. 缓存一致性如何支撑原子操作

原子操作不是只靠一条指令名字完成的，它依赖 CPU 的缓存一致性协议。

多核系统中，每个核心有自己的缓存：

```text
Core 0 L1 Cache
Core 1 L1 Cache
Core 2 L1 Cache
共享内存
```

如果 `state_` 所在缓存行被多个核心访问，CPU 必须保证所有核心对该缓存行的修改顺序一致。

常见协议是 MESI 或其变体：

- Modified：当前核心拥有修改后的缓存行。
- Exclusive：当前核心独占干净缓存行。
- Shared：多个核心共享只读缓存行。
- Invalid：缓存行失效。

当一个核心要执行原子写或 CAS 时，它需要获得缓存行的独占权限。

竞争严重时，缓存行会在核心之间来回迁移：

```text
Core 0 CAS state_
Core 1 CAS state_
Core 2 CAS state_
Core 0 再次 CAS state_
```

这就是自旋锁高竞争时性能下降的重要原因。

## 11. 内存屏障和编译器屏障

内存序不仅约束 CPU，也约束编译器。

编译器优化可能会：

- 调整普通读写顺序。
- 把变量缓存到寄存器。
- 删除它认为无用的重复读取。
- 合并相邻写入。

CPU 也可能会：

- 乱序执行。
- 使用 store buffer 延迟写入可见性。
- 先执行后面的 load。

原子操作的内存序告诉编译器和 CPU：哪些重排不能做。

### 11.1 acquire 防止什么

```cpp
lock();
shared_data = shared_value;
```

加锁成功的 acquire 保证：

```text
临界区内的读写不能被移动到 lock 成功之前
```

否则线程可能还没真正获得锁，就先访问共享数据。

### 11.2 release 防止什么

```cpp
shared_value = 100;
unlock();
```

解锁的 release 保证：

```text
临界区内的读写不能被移动到 unlock 之后
```

否则其他线程可能看到锁已经释放，却看不到共享数据的新值。

## 12. 本项目各原子操作的作用清单

### 12.1 Mutex::lock

```cpp
locked_.test_and_set(std::memory_order_acquire)
```

作用：

- 原子地把锁状态设置为已占用。
- 返回旧状态，用于判断是否获得锁。
- acquire 保证成功后进入临界区能看到之前 release 解锁线程的写入。

### 12.2 Mutex::try_lock

```cpp
!locked_.test_and_set(std::memory_order_acquire)
```

作用：

- 尝试一次。
- 成功则当前线程持有锁。
- 失败则不等待。

### 12.3 Mutex::unlock

```cpp
locked_.clear(std::memory_order_release)
```

作用：

- 原子地释放锁。
- release 发布临界区内的数据修改。

### 12.4 SharedMutex::lock

```cpp
const unsigned long long my_ticket =
    next_ticket_.fetch_add(1, std::memory_order_acq_rel);

state_.compare_exchange_weak(expected_state, -1,
                             std::memory_order_acquire,
                             std::memory_order_relaxed);
```

作用：

- `next_ticket_.fetch_add` 给写线程分配统一 FIFO ticket。
- CAS 把空闲状态改成写锁状态。

### 12.5 SharedMutex::try_lock

```cpp
next_ticket_.compare_exchange_strong(expected_next_ticket,
                                     serving_ticket + 1,
                                     std::memory_order_acq_rel,
                                     std::memory_order_relaxed);
state_.compare_exchange_strong(expected_state, -1,
                               std::memory_order_acquire,
                               std::memory_order_relaxed);
```

作用：

- 只在 `state_ == 0` 时获得写锁。
- 不阻塞等待。
- 不插队到已经领取统一 ticket 的读/写请求前面。
- 如果预留 ticket 后没有成功拿到写锁，会推进 `serving_ticket_`，避免后续请求卡住。

### 12.6 SharedMutex::unlock

```cpp
state_.store(0, std::memory_order_release);
serving_ticket_.fetch_add(1, std::memory_order_acq_rel);
```

作用：

- 释放写锁。
- 让后续读线程或写线程能观察到空闲状态。
- 推进统一 ticket，让下一个读/写请求有资格竞争。

### 12.7 SharedMutex::lock_shared

```cpp
serving_ticket_.load(std::memory_order_acquire);
state_.load(std::memory_order_acquire);
state_.compare_exchange_weak(current_state, current_state + 1,
                             std::memory_order_acquire,
                             std::memory_order_relaxed);
serving_ticket_.fetch_add(1, std::memory_order_acq_rel);
```

作用：

- 先等待自己的统一 ticket。
- 再观察是否有写线程持锁。
- CAS 成功后读者数量加 1。
- 读线程进入后推进 ticket，让后续连续读线程可以组成并发读批次。

### 12.8 SharedMutex::try_lock_shared

```cpp
next_ticket_.compare_exchange_strong(expected_next_ticket,
                                     serving_ticket + 1,
                                     std::memory_order_acq_rel,
                                     std::memory_order_relaxed);
state_.compare_exchange_weak(current_state, current_state + 1,
                             std::memory_order_acquire,
                             std::memory_order_relaxed);
serving_ticket_.fetch_add(1, std::memory_order_acq_rel);
```

作用：

- 只在没有其他读/写请求排队时尝试进入。
- 如果已经有请求排队，直接失败，不插队。
- 成功后共享读者数量增加。
- 成功进入后推进 ticket，让后续请求继续。

### 12.9 SharedMutex::unlock_shared

```cpp
state_.fetch_sub(1, std::memory_order_release);
```

作用：

- 读者数量减 1。
- 最后一个读者退出后，`state_` 变成 0，写线程可进入。

## 13. weak 和 strong 的选择原则

本项目选择：

- 阻塞重试循环使用 `compare_exchange_weak`。
- 非阻塞只尝试一次的 `try_lock` 使用 `compare_exchange_strong`。

原因：

```text
weak:
    允许虚假失败
    适合循环
    可能更贴近硬件

strong:
    不应虚假失败
    适合只尝试一次的逻辑
```

注意：即使 `strong` 失败，也不代表一定有 bug。它表示原子变量当前值和 expected 不相等。

## 14. ABA 问题说明

CAS 有一个经典问题叫 ABA。

场景：

```text
线程 A 读取值 A
线程 B 把值 A 改成 B
线程 B 又把值 B 改回 A
线程 A CAS 发现还是 A，于是认为状态没变过
```

本项目读写锁不涉及指针节点回收，也不根据“状态没有变化过”做资源生命周期判断，因此 ABA 风险较小。

但在无锁栈、无锁队列等结构里，ABA 可能非常严重，通常需要：

- 版本号。
- tagged pointer。
- hazard pointer。
- epoch-based reclamation。

这也是锁实现比完整无锁容器更适合入门展示的原因。

## 15. 原子操作不是万能的

原子操作解决的是单个或少量状态变量的并发修改问题。它不能自动解决所有并发问题。

需要额外考虑：

- 状态机是否完整。
- 是否存在饥饿。
- 是否需要公平队列。
- 是否会忙等浪费 CPU。
- 是否需要阻塞等待。
- 是否需要超时和取消。
- 是否存在错误 unlock。
- 是否需要 owner 记录。

本项目的价值在于把锁的核心状态机和原子操作关系写清楚，而不是替代标准库生产锁。

## 16. 面试讲解建议

讲本项目原子操作时，可以按这个顺序：

1. 普通变量不能实现锁，因为检查和修改不是原子的。
2. CPU 提供原子读改写指令。
3. C++11 用 `std::atomic` 抽象 CPU 原子能力。
4. `atomic_flag::test_and_set` 可以实现最小互斥锁。
5. `atomic<int>` + CAS 可以实现读写锁状态机。
6. acquire/release 保证临界区数据可见性。
7. 高竞争下自旋锁的瓶颈来自缓存行争用。
8. Linux 生产锁通常会在竞争严重时用 futex 进入内核睡眠。
