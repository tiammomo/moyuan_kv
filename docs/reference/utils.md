# 工具层

## 1. `RWLock`

**对应文件**：`mokv/utils/lock.hpp`

当前 `RWLock` 是一个基于 `pthread_rwlock_t` 的 RAII 包装，提供：

- `ReadLock`
- `WriteLock`

主要被这些地方使用：

- `DB`
- `Manifest`

和跳表不同，跳表当前没有继续使用 `RWLock`，而是直接用了 `std::atomic_flag`。

## 2. `ThreadPool`

**对应文件**：`mokv/pool/thread_pool.hpp`

当前线程池位于命名空间：

```cpp
namespace cpputil::pool
```

主要接口包括：

- `Enqueue(...)`
- `MultiEnqueue(...)`
- `ConcurrentRun(...)`
- `IsBusy()`

它是一个简单的任务队列线程池，不是 work-stealing 实现。

## 3. `RingBufferQueue`

**对应文件**：`mokv/utils/ring_buffer_queue.hpp`

当前命名空间是：

```cpp
namespace cpputil::pbds
```

它提供：

- `PushBack`
- `PopFront`
- `PopBack`
- `Truncate`
- `At`
- `RAt`

当前主要用于 `RaftLog` 的内存 entry 队列。

## 4. `GlobalRand`

**对应文件**：

- `mokv/utils/global_random.h`
- `mokv/utils/global_random.cpp`

当前实现非常直接：

- 一个原子递增计数器
- 每次 `fetch_add(1, memory_order_relaxed)`

它现在主要用于跳表随机层级生成。

## 5. `codec`

**对应文件**：`mokv/utils/codec.hpp`

这是当前文档里最容易漏掉、但上层 LLM 访问层高度依赖的工具头。

它提供：

- `Escape(...)`
- `Unescape(...)`
- `EscapeKeyPart(...)`
- `Split(...)`
- `ParseInt64(...)`
- `ParseUInt32(...)`
- `JoinKeyParts(...)`

`llm/store.hpp` 中的 keyspace 和文本序列化都依赖这里。

## 6. `ResourceManager`

**对应文件**：`mokv/resource_manager.hpp`

当前 `ResourceManager` 负责保管：

- `raft::ConfigManager`
- `DB`
- `raft::Pod`

初始化顺序大致是：

1. 构造时加载 `raft.cfg`
2. `InitDb()`
3. `InitPod()`

因此，server 路径下对配置文件路径的依赖，也集中在这里开始生效。

## 当前需要知道的事实

- 工具层命名空间并不统一：
  - `mokv::common`
  - `cpputil::pool`
  - `cpputil::pbds`
- `codec.hpp` 已经是当前上层 keyspace 设计的一部分
- `RWLock` 仍然在 DB / Manifest 中使用，但跳表并没有继续使用它
