# MemTable

**对应文件**：`mokv/lsm/memtable.hpp`

## 当前角色

`MemTable` 是 `DB` 的活跃内存层。它本身非常薄，只做两件事：

- 把有序存储委托给 `ConcurrentSkipList`
- 提供统一接口给 `DB`

```cpp
class MemTable {
public:
    bool Get(std::string_view key, std::string& value);
    void Put(std::string_view key, std::string_view value);
    void Delete(std::string_view key);
    size_t binary_size();
    size_t size();
    Iterator begin();
    Iterator end();
};
```

仓库里还保留了一个兼容别名：

```cpp
using MemeTable = MemTable;
```

这是为了兼容旧代码里的拼写错误。

## 它不负责什么

`MemTable` 当前不负责：

- 自动刷盘
- 后台线程调度
- tombstone 解释
- BlockCache
- Manifest 更新

这些都在 `DB` 层处理。

## 数据流中的位置

```text
Put
  -> DB
  -> active MemTable
  -> flush_queue_
  -> SST
```

```text
Get
  -> DB
  -> active MemTable
  -> flush_queue_ 中尚未落盘的 MemTable
  -> Manifest / SST
```

## 和删除的关系

`MemTable::Delete()` 只是把删除请求继续转给底层跳表。

但在当前系统里，真正暴露给上层的删除行为主要走的是：

```cpp
DB::Delete(key) -> DB::Put(key, TombstoneValue())
```

也就是说，DB 层更依赖 tombstone 来跨 MemTable / SST 表达删除，而不是要求每次都直接把 key 从底层结构物理擦掉。

## 刷盘是怎么发生的

`MemTable` 本身没有刷盘线程。当前实际流程在 `DB` 里：

1. 活跃 `memtable_` 写入
2. 超过 `memtable_max_size`
3. 旧 `memtable_` 被转移到 `flush_queue_`
4. 后台线程调用 `ToSST(...)`
5. 生成新的 `SST`

## 当前测试覆盖

虽然没有单独的 `memtable_test`，但它已经被这些测试间接覆盖：

- `skip_list_test`
- `db_test`
- `compaction_test`
- `compression_test`

因此，学习 MemTable 时最好结合 [db-layer.md](db-layer.md) 一起看。
