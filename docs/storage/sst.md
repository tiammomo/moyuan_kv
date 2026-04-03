# SST

**对应文件**：`mokv/lsm/sst.hpp`

## 当前实现概览

SST 是 `mokv` 的磁盘有序表，负责把 MemTable 中的有序键值对持久化为 `.sst` 文件。

当前实现的几个关键点：

- 文件读写基于 `mmap`
- 读取先走 `IndexBlock` 定位 DataBlock
- DataBlock 自带 BloomFilter
- DataBlock 内的 entries 可以按块压缩
- 压缩块带自描述头，读路径可以自动识别

## 文件结构

高层结构仍然是：

```text
[IndexBlock][DataBlock 0][DataBlock 1]...[DataBlock N]
```

### IndexBlock

IndexBlock 负责：

- 记录每个 DataBlock 的偏移量
- 记录每个 DataBlock 的起始 key
- 让查找路径先做一次块级二分

### DataBlock

DataBlock 负责：

- 保存一个 block 内的若干键值对
- 保存 BloomFilter
- 在需要时保存压缩后的 entries

## 当前压缩块格式

这一版和旧文档最大的不同，是压缩块不再靠“外部约定”解释，而是自带头信息。

DataBlock payload 当前包含：

- `magic`
- `version`
- `flags`
- `compression_type`
- `entries_size`
- `bloom_filter`
- `entries` 或 `compressed_entries`

也就是说，读取路径可以在 `LoadEncodedBlock(...)` 中做到：

1. 识别是不是 encoded block
2. 识别是否真的压缩了
3. 识别压缩算法类型
4. 决定是否解压后再解析 entries

## LZ4 互通

DataBlock 中如果使用 `kLZ4`：

- 压缩器来自 `mokv/utils/compression.hpp`
- 实际走的是 `liblz4` frame API

因此，SST 里的“压缩 entries payload”已经能和外部 `liblz4` frame 互通。

要注意：

- 互通的是 block 内部的 LZ4 frame
- 不是整个 `.sst` 文件可以直接当成 `.lz4` 文件来处理

## 读取流程

当前 `SST::Get()` 的核心步骤可以概括为：

1. 通过 `IndexBlock` 找到目标 block
2. 读取对应 DataBlock
3. 先用 BloomFilter 过滤
4. 如果 block 是压缩块，先解压 entries
5. 再在 block 内做二分查找

如果配置了 BlockCache，还会优先查缓存。

## BlockCache 与预取

SST 已经提供了 block 预取相关能力，例如：

- `PrefetchDataBlock(...)`
- `PrefetchDataBlocks(...)`
- `PrefetchAllDataBlocks()`

这部分的作用是：

- 把 mmap 区域中的 block 预先解析进缓存
- 降低后续热点查询的代价

## 当前实现里值得注意的点

### 1. `EntryIndex` 仍然引用 block 内存

无压缩块时，它直接引用 `mmap` 出来的文件内存；
压缩块时，它引用解压后的 `entry_storage_`。

### 2. 只有压缩收益足够时才真的压缩

`CompressedBlockBuilder` 会比较压缩前后的大小：

- 如果压缩后不更小，就保留未压缩 entries
- 同时仍然保留统一的自描述 block 头

### 3. 构造路径区分“启用压缩”和“实际压缩成功”

`CompressionConfig.enable = true` 只是允许压缩；
最终 `SST::compressed_` 取决于实际 block 是否采用了压缩形式。

## 当前测试覆盖

- `compression_test`
- `compaction_test`
- `db_test`

其中 `compression_test` 已经覆盖：

- 压缩 SST 重载
- DB 重启后读取压缩数据
- `liblz4` 双向互通
