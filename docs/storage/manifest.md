# Manifest

**对应文件**：`mokv/lsm/manifest.hpp`

## 它管什么

`Manifest` 负责管理所有 SST 的层级关系和版本视图：

- 记录每一层有哪些 SST
- 按层读取数据
- 触发和执行 size-tiered compaction
- 持久化 `manifest` 文件

## 当前持久化格式

Manifest 文件当前保存：

```text
[version][level_count][level0 sst ids ... -1][level1 sst ids ... -1]...
```

每层以 `-1` 作为结束标记。

## Copy-on-Write

当前 Manifest 的更新方式是 Copy-on-Write：

- 旧版本继续可读
- 新插入或 compaction 时复制出新版本
- `DB` 再把新版本放入 `manifest_queue_`

这也是 `InsertAndUpdate(...)` 的核心作用。

## Level 行为

### Level 0

- 新刷盘的 SST 直接插入到这里
- key 范围允许重叠
- 读取时要按“新的优先”逆序查找

### Level 1 及以上

- compaction 后尽量保持 key 区间不重叠
- 读取时可以按块起始 key 做二分定位

## 当前 compaction 策略

`mokv` 现在采用的是按层总大小触发的 size-tiered compaction。

层级阈值目前写死在 `level_max_binary_size(level)` 里：

| 层级 | 阈值 |
|------|------|
| L0 | `1 KB` |
| L1 | `10 MB` |
| L2 | `100 MB` |
| L3 | `1 GB` |
| L4 | `10 GB` |

`CanDoCompaction()` 当前就是检查 L0 是否超过阈值。

## compaction 做了什么

`SizeTieredCompaction(...)` 的核心流程是：

1. 取出当前层的 SST
2. 找出下一层中 key 范围重叠的 SST
3. 用优先队列做多路归并
4. 相同 key 只保留最新值
5. 根据传入的 `CompressionConfig` 生成新的 SST
6. 清空当前层，重建下一层 SST 列表

## 压缩配置是如何传下去的

这点是当前分支已经修好的地方：

- `DB::ToSST(...)` 会把 `config_.compression` 传给 SST 构造
- `DB::ToSST(...)` 触发 compaction 时，也会把同一份 `CompressionConfig` 传给 Manifest
- 所以 compaction 产出的新 SST 会沿用当前 DB 压缩配置

## 读取顺序

`Manifest::Get()` 当前是从低层到高层查找：

1. 先看 Level 0
2. 再看 Level 1
3. 依次向上

因为低层通常代表更新、更近写入的数据。

## 当前测试覆盖

- `compaction_test`
- `db_test`
- `compression_test`

这些测试已经覆盖了：

- Manifest 加载 / 保存
- 插入 SST 后读取
- compaction 后读取
- 压缩配置透传后的读取
