# BlockCache 块缓存

**文件**: `lsm/block_cache.hpp`

LRU 缓存，用于缓存 SST 文件的 DataBlock，减少磁盘 I/O。

## 设计目标

- 缓存热点 DataBlock
- 支持 LRU 淘汰策略
- 多线程安全

## 核心数据结构

### CacheBlock

```cpp
struct CacheBlock {
    std::vector<char> data_;   // 块数据
    size_t sst_id_;            // SST 文件 ID
    size_t block_offset_;      // 块在 SST 中的偏移量
    uint64_t access_time_;     // 访问时间戳
    size_t hit_count_{0};      // 命中次数
};
```

### BlockCache 核心字段

```cpp
class BlockCache {
private:
    Config config_;                           // 缓存配置
    std::unordered_map<Key, std::list<CacheBlock>::iterator> cache_map_;
    std::list<CacheBlock> cache_list_;        // LRU 链表（头部最新）
    std::mutex mutex_;                        // 互斥锁
    size_t current_size_{0};                  // 当前缓存大小
    size_t hit_count_{0};                     // 命中次数
    size_t miss_count_{0};                    // 未命中次数
};
```

### 配置

```cpp
struct Config {
    size_t max_capacity = 256 * 1024 * 1024;  // 最大容量 256MB
    size_t min_block_size = 4096;             // 最小块大小
    size_t max_block_size = 64 * 1024;        // 最大块大小 64KB
    size_t max_block_count = 0;               // 块数量限制（0=不限制）
    double min_utilization = 0.5;             // 最小利用率
};
```

## 缓存键设计

```cpp
using Key = uint64_t;

static Key MakeKey(size_t sst_id, size_t block_offset) {
    // 使用 sst_id 作为高位，block_offset 作为低位
    return (static_cast<Key>(sst_id) << 32) | static_cast<Key>(block_offset);
}

static size_t GetSSTId(Key key) {
    return static_cast<size_t>(key >> 32);
}

static size_t GetBlockOffset(Key key) {
    return static_cast<size_t>(key & 0xFFFFFFFF);
}
```

## 核心方法

### 1. 获取缓存 (Get)

```cpp
std::shared_ptr<CacheBlock> Get(Key key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_map_.find(key);
    if (it == cache_map_.end()) {
        ++miss_count_;
        return nullptr;  // 未命中
    }

    // 命中：移动到链表头部（最新）
    auto& iter = it->second;
    auto block = *iter;

    // 更新访问时间
    block.access_time_ = GetCurrentTimeMs();
    ++block.hit_count_;

    // 移动到头部
    cache_list_.splice(cache_list_.begin(), cache_list_, iter);

    ++hit_count_;
    return std::make_shared<CacheBlock>(block);
}
```

### 2. 放入缓存 (Put)

```cpp
void Put(Key key, std::string_view data) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 检查是否已存在
    auto it = cache_map_.find(key);
    if (it != cache_map_.end()) {
        // 已存在，更新数据并移动到头部
        auto& iter = it->second;
        iter->data_.assign(data.data(), data.size());
        cache_list_.splice(cache_list_.begin(), cache_list_, iter);
        return;
    }

    // 插入新块
    CacheBlock block;
    block.data_.assign(data.data(), data.size());
    block.sst_id_ = GetSSTId(key);
    block.block_offset_ = GetBlockOffset(key);
    block.access_time_ = GetCurrentTimeMs();

    current_size_ += data.size();

    // 添加到链表头部
    cache_list_.push_front(std::move(block));
    auto iter = cache_list_.begin();
    cache_map_[key] = iter;

    // 淘汰旧块
    EvictIfNeeded();
}
```

### 3. 淘汰策略 (EvictIfNeeded)

```cpp
void EvictIfNeeded() {
    // 淘汰直到容量合适
    while (current_size_ > config_.max_capacity) {
        if (cache_list_.empty()) break;

        // 从链表尾部淘汰（最旧）
        auto iter = --cache_list_.end();
        auto& block = *iter;

        // 生成缓存键
        auto key = MakeKey(block.sst_id_, block.block_offset_);
        cache_map_.erase(key);

        current_size_ -= block.data_.size();
        cache_list_.erase(iter);
    }

    // 数量限制检查
    if (config_.max_block_count > 0) {
        while (cache_map_.size() > config_.max_block_count) {
            auto iter = --cache_list_.end();
            auto& block = *iter;
            auto key = MakeKey(block.sst_id_, block.block_offset_);
            cache_map_.erase(key);
            cache_list_.erase(iter);
        }
    }
}
```

## LRU 策略说明

```
LRU 链表结构（头部最新，尾部最旧）:

cache_list_: [MRU] ← → [Recent] ← → ... ← → [Old] ← → [LRU]

Put/Get 命中时:
  [A] ← → [B] ← → [C]
              ↑
           访问 C
              ↓
  [C] ← → [A] ← → [B]  ← C 移动到头部

淘汰时:
  [A] ← → [B] ← → [C]
                      ↑
                 淘汰 C
```

## 缓存命中率统计

```cpp
struct Stats {
    size_t hit_count;
    size_t miss_count;
    double hit_rate;
    size_t current_size;
    size_t item_count;
};

Stats GetStats() const {
    size_t total = hit_count_ + miss_count_;
    return {
        hit_count_,
        miss_count_,
        total > 0 ? static_cast<double>(hit_count_) / total : 0,
        current_size_,
        cache_map_.size()
    };
}
```

## 在 SST 中的应用

```cpp
class SST {
private:
    BlockCache* block_cache_{nullptr};

public:
    void SetBlockCache(BlockCache* cache) {
        block_cache_ = cache;
    }

    std::string_view GetDataBlock(size_t block_index) {
        auto key = BlockCache::MakeKey(id_, block_index);

        // 1. 尝试从缓存获取
        if (block_cache_) {
            auto cached = block_cache_->Get(key);
            if (cached != nullptr) {
                return std::string_view(
                    cached->data_.data(),
                    cached->data_.size()
                );
            }
        }

        // 2. 未命中，从 mmap 加载
        auto offset = index_block.GetBlockOffset(block_index);
        auto size = GetBlockSize(block_index);
        std::string_view block_data(data_ + offset, size);

        // 3. 放入缓存
        if (block_cache_) {
            block_cache_->Put(key, block_data);
        }

        return block_data;
    }
};
```

## 特性总结

| 特性 | 说明 |
|------|------|
| 缓存粒度 | DataBlock |
| 淘汰策略 | LRU |
| 线程安全 | mutex 保护 |
| 最大容量 | 256MB（可配置） |
| 命中率统计 | 支持 |

## 与其他缓存对比

| 特性 | BlockCache | ConcurrentLRUCache |
|------|------------|-------------------|
| 存储内容 | DataBlock | 任意对象 |
| 线程安全 | mutex | boost::concurrent_flat_map |
| 淘汰策略 | LRU | LRU + Lazy Promotion |
| 适用场景 | SST 块缓存 | 通用对象缓存 |

## 测试验证

| 测试项 | 状态 |
|--------|------|
| cache_test | PASS |

> 性能指标：亚毫秒级延迟
