# DB 层

**对应文件**：`mokv/db.hpp`

## 当前职责

`DB` 是仓库里最核心的存储协调层，连接了：

- `MemTable`
- `SST`
- `Manifest`
- `BlockCache`
- 后台刷盘线程

上层无论走 `DBKVStore` 还是 `RaftLog -> DB`，最终都会落到这里。

## 当前配置结构

```cpp
struct DBConfig {
    lsm::CompressionConfig compression;
    lsm::BlockCache::Config block_cache;
    size_t memtable_max_size;
    bool enable_block_cache;
};
```

当前默认行为是：

- 压缩开启
- BlockCache 开启
- `memtable_max_size` 默认约 `3 MB`

## 关键成员

```cpp
class DB {
private:
    DBConfig config_;
    std::shared_ptr<lsm::MemTable> memtable_;
    std::vector<std::shared_ptr<lsm::Manifest>> manifest_queue_;
    common::RWLock manifest_lock_;
    common::RWLock memtable_lock_;

    std::unique_ptr<lsm::BlockCache> block_cache_;

    std::thread to_sst_thread_;
    std::mutex flush_mutex_;
    std::condition_variable to_sst_cv_;
    std::vector<std::shared_ptr<lsm::MemTable>> flush_queue_;
    bool to_sst_stop_flag_ = false;

    std::atomic_size_t sst_id_{0};
};
```

和旧版文档相比，当前真实成员是 `flush_queue_`，不再是历史文档里的 `inmemtables_`。

## 删除语义

`DB::Delete()` 当前不会直接从磁盘层移除 key，而是写入 tombstone：

```cpp
Put(key, detail::TombstoneValue())
```

读取路径会统一识别 tombstone，并对外表现为“key 不存在”。

这让删除语义能跨：

- active MemTable
- flush_queue_ 中待落盘数据
- 已持久化 SST

保持一致。

## 写入流程

```text
DB::Put
  -> 写 active memtable_
  -> 超过 memtable_max_size 时切换出旧 memtable_
  -> 旧 memtable 进入 flush_queue_
  -> 唤醒后台线程
```

后台线程 `ToSSTLoop()` 会持续消费 `flush_queue_`，并调用 `ToSST(...)`：

1. 生成新的 SST
2. 挂接 BlockCache
3. `Manifest::InsertAndUpdate(...)`
4. 满足条件时执行 compaction

## 读取流程

当前 `DB::Get()` 顺序是：

1. `memtable_`
2. `flush_queue_` 中尚未刷盘的 MemTable，按新到旧逆序检查
3. `manifest_queue_.back()`

每一步都会统一处理 tombstone。

## 生命周期

### 构造

构造时会：

- 创建 active `MemTable`
- 启动后台刷盘线程
- 创建首个 `Manifest`
- 按需创建 `BlockCache`

### 析构

析构时会：

1. 把仍有数据的 active `MemTable` 转入待刷盘队列
2. 停止后台线程并等待退出
3. 保存最新 `Manifest`

所以当前 DB 的析构路径本身就承担了“尽量把内存数据刷干净”的责任。

## 压缩和 compaction

当前 `DB` 已经把压缩配置贯穿到两条路径：

- 正常刷盘生成 SST
- Manifest compaction 生成 SST

也就是说：

- 默认压缩开启
- compaction 不会丢失压缩配置

## BlockCache

`DB` 还暴露了几组缓存统计接口：

- `GetCacheStats()`
- `GetCacheHitRate()`
- `ClearCache()`
- `GetCacheSize()`

这些接口当前主要适合库内调试和观测。

## 一个需要知道的细节

`GetCompressionRatio()` 现在是粗略估算值，不是精确统计值。它会把 `uncompressed_size` 按近似值推算，所以文档和上层展示不应该把它当成精确压缩率指标。

## 当前测试覆盖

- `db_test`
- `compression_test`
- `compaction_test`
- `llm_store_test` 间接覆盖 `DBKVStore -> DB`

截至 `2026-04-03`，这些路径当前都处于通过状态。
