# 缓存系统

## 概述

项目包含两级缓存系统：
1. **BlockCache**: SST DataBlock 缓存（位于 lsm/）
2. **Concurrent2LRUCache**: 通用对象缓存（位于 cache/）

---

## 1. ConcurrentLRUCache

**文件**: `cache/concurrent_cache.hpp`

线程安全的 LRU 缓存，使用 boost::concurrent_flat_map 和双向链表实现。

### 核心数据结构

```cpp
template <typename TKey, typename TValue>
class ConcurrentLRUCache {
private:
    struct Node {
        TKey key;
        TValue value;
        TNode* pre;
        TNode* next;
        std::atomic<size_t> promotions;  // 提升计数
        uint64_t access_time;            // 访问时间
    };

    boost::concurrent_flat_map<TKey, TNode*> map_;  // 线程安全哈希表
    TNode* head_;                                   // 链表头部（最新）
    TNode* tail_;                                   // 链表尾部（最旧）
    std::mutex mutex_;                              // 互斥锁
    size_t max_size_;                               // 最大容量
};
```

### 关键优化：延迟提升 (Lazy Promotion)

```cpp
class ConcurrentLRUCache {
private:
    constexpr static size_t should_promote_num_ = 10;  // 提升阈值

public:
    inline bool ShouldPromote(TNode* node) {
        // 只有访问次数达到阈值才真正移动到链表头部
        return node->promotions.fetch_add(1, std::memory_order_acq_rel) >= should_promote_num_;
    }

    std::shared_ptr<TValue> Get(const TKey& key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return nullptr;
        }

        TNode* node = it->second;

        // 延迟提升
        if (ShouldPromote(node)) {
            std::lock_guard<std::mutex> lock(mutex_);
            MoveToHead(node);
        }

        return std::make_shared<TValue>(node->value);
    }
};
```

**延迟提升的优势**:
- 减少锁竞争
- 避免热点数据频繁移动
- 只提升真正热的数据

---

## 2. Concurrent2LRUCache (W-TinyLFU 变体)

**文件**: `cache/concurrent_2_lru_cache.hpp`

两级缓存结构，模拟 W-TinyLFU 策略。

### 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                    Concurrent2LRUCache                       │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              Window LRU (1%)                         │   │
│  │  - 快速接收新数据                                     │   │
│  │  - 保护主缓存不被突发流量冲垮                         │   │
│  │  - 容量小（缓存容量的 1%）                            │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                             │
│         ↓ 淘汰时检查频率                                     │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              Main LRU (99%)                          │   │
│  │  - 存储热点数据                                       │   │
│  │  - 容量大                                             │   │
│  │  - 使用 CMSketch 统计频率                             │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              CMSketch (频率估计)                     │   │
│  │  - Count-Min Sketch 4-bits                           │   │
│  │  - 高效估计元素访问频率                               │   │
│  │  - 用于淘汰决策                                       │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 核心代码

```cpp
template <typename TKey, typename TValue>
class Concurrent2LRUCache {
private:
    static constexpr double kWindowRatio = 0.01;  // 窗口占比 1%
    ConcurrentLRUCache<TKey, TValue>* window_lru_;  // 窗口缓存
    ConcurrentLRUCache<TKey, TValue>* main_lru_;    // 主缓存
    utils::CMSketch4Bits<TKey>* cm_sketch_;         // 频率估计器
    size_t max_size_;                               // 最大容量

public:
    void Put(const TKey& key, const TValue& value) {
        // 1. 更新频率
        cm_sketch_->Increment(key);

        // 2. 尝试放入窗口缓存
        if (window_lru_->Put(key, value)) {
            return;
        }

        // 3. 窗口缓存已满，需要淘汰
        EvictFromWindow(key);
    }

    std::shared_ptr<TValue> Get(const TKey& key) {
        // 1. 频率统计
        cm_sketch_->Increment(key);

        // 2. 查找窗口缓存
        auto value = window_lru_->Get(key);
        if (value != nullptr) {
            return value;
        }

        // 3. 查找主缓存
        return main_lru_->Get(key);
    }

private:
    void EvictFromWindow(const TKey& new_key) {
        // 从窗口缓存淘汰一个元素
        auto evicted = window_lru_->PopTail();
        if (evicted == nullptr) return;

        // 比较频率决定是否进入主缓存
        auto new_freq = cm_sketch_->Estimate(evicted->key);
        auto old_freq = cm_sketch_->Estimate(new_key);

        if (new_freq < old_freq) {
            // 新元素频率更高，淘汰旧元素
            return;
        } else {
            // 新元素频率更低，放入主缓存
            main_lru_->Put(evicted->key, evicted->value);
        }
    }
};
```

---

## 3. CMSketch (Count-Min Sketch)

**文件**: `cache/cm_sketch.hpp`

Count-Min Sketch 用于高效的频率估计，4-bit 计数器，支持最大计数 15。

### 原理

```
┌─────────────────────────────────────────────────────────────┐
│                    CMSketch 结构                            │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  元素 "hello" 的哈希分布:                                    │
│                                                             │
│  Hash1: "hello" → 7   → data[0][7 % 8]  = [7]              │
│  Hash2: "hello" → 3   → data[1][3 % 8]  = [3]              │
│                                                             │
│  数据数组:                                                  │
│  ┌───────┬───────┬───────┬───────┬───────┬───────┬───┬───┐ │
│  │ Shard0│ Shard1│ Shard2│ Shard3│ ...   │       │   │   │ │
│  ├───────┼───────┼───────┼───────┼───────┼───────┼───┼───┤ │
│  │ [0..7]│ [0..7]│ [0..7]│ [0..7]│       │       │   │   │ │
│  └───────┴───────┴───────┴───────┴───────┴───────┴───┴───┘ │
│                                                             │
│  估计频率 = min(data[0][hash0], data[1][hash1], ...)        │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 核心代码

```cpp
template <typename T, size_t Shard = 2>
class CMSketch4Bits {
private:
    uint8_t* data_[Shard];      // 2个分片的计数数组
    uint64_t seed_[Shard];      // 哈希种子
    const size_t capacity_;     // 数组容量
    const size_t capacity_mask_;

public:
    CMSketch4Bits(size_t capacity) : capacity_(capacity) {
        size_t size = (capacity + 1) / 2;  // 4-bit per counter

        for (size_t i = 0; i < Shard; ++i) {
            data_[i] = new uint8_t[size]();
            seed_[i] = GlobalRand();
        }

        capacity_mask_ = capacity - 1;
    }

    ~CMSketch4Bits() {
        for (size_t i = 0; i < Shard; ++i) {
            delete[] data_[i];
        }
    }

    // 增加计数
    void Increment(const T& key) {
        for (size_t i = 0; i < Shard; ++i) {
            size_t pos = (Hash(key, seed_[i]) & capacity_mask_) / 2;
            uint8_t& counter = data_[i][pos];
            if (counter < 15) {  // 4-bit 最大值
                ++counter;
            }
        }
    }

    // 估计频率
    size_t Estimate(const T& key) const {
        size_t min_count = SIZE_MAX;

        for (size_t i = 0; i < Shard; ++i) {
            size_t pos = (Hash(key, seed_[i]) & capacity_mask_) / 2;
            min_count = std::min(min_count, static_cast<size_t>(data_[i][pos]));
        }

        return min_count;
    }

private:
    static uint64_t Hash(const T& key, uint64_t seed) {
        // 使用 std::hash 和种子混合
        size_t h = std::hash<T>{}(key);
        return seed ^ (h + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    }
};
```

### 特性

| 特性 | 说明 |
|------|------|
| 空间效率 | 每个元素只需 4-bit |
| 查询时间 | O(1)，O(Shard) 次哈希 |
| 频率下界 | 返回真实频率的下界 |
| 假阳性 | 可能高估频率（不会低估） |
| 更新次数 | 限制为 15 次 |

---

## 缓存策略对比

| 策略 | 优点 | 缺点 | 适用场景 |
|------|------|------|----------|
| LRU | 实现简单，热门数据保留 | 突发流量会冲刷热点 | 读多写少 |
| LFU | 精确保留热点 | 需要维护频率，内存开销大 | 访问分布稳定 |
| W-TinyLFU | 平衡新数据和热点 | 实现复杂 | 混合负载 |
| Clock | 类似 LRU，更高并发 | 精度略低 | 高并发场景 |

---

## 缓存一致性

```
┌─────────────────────────────────────────────────────────────┐
│                    缓存一致性模型                            │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  写入策略:                                                  │
│                                                             │
│  1. Write-Through (写穿透)                                  │
│     - 同步写入缓存和底层存储                                 │
│     - 简单，但写入延迟高                                     │
│                                                             │
│  2. Write-Back (写回)                                       │
│     - 只写入缓存，延迟批量刷盘                               │
│     - 复杂，有数据丢失风险                                   │
│                                                             │
│  本项目采用 Write-Through:                                  │
│  - DB.Put() → MemTable → SST → Manifest                    │
│  - BlockCache 只读，不参与写入                               │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## 测试验证

| 测试项 | 状态 |
|--------|------|
| cm_sketch_test | PASS |

> CMSketch 频率估计功能已验证
