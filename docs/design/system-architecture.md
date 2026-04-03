# 架构设计

## 当前分层

```text
┌─────────────────────────────────────────────┐
│ CLI / gRPC Client                           │
└──────────────────────┬──────────────────────┘
                       │
┌──────────────────────▼──────────────────────┐
│ KV API / LLM API                            │
│ DBKVStore / LLMStore                        │
└──────────────────────┬──────────────────────┘
                       │
┌──────────────────────▼──────────────────────┐
│ DB                                           │
│ active memtable + flush queue + manifest     │
└──────────────────────┬──────────────────────┘
                       │
┌──────────────────────▼──────────────────────┐
│ LSM Engine                                   │
│ SkipList -> MemTable -> SST -> Manifest      │
└──────────────────────┬──────────────────────┘
                       │
┌──────────────────────▼──────────────────────┐
│ 辅助层                                       │
│ BloomFilter / BlockCache / Compression       │
└──────────────────────┬──────────────────────┘
                       │
┌──────────────────────▼──────────────────────┐
│ 服务端路径                                   │
│ ResourceManager / Pod / RaftLog / Service    │
└──────────────────────────────────────────────┘
```

## 两条主要调用链

### 1. 嵌入式调用链

```text
业务代码
  -> DBKVStore
  -> DB
  -> MemTable / Manifest / SST
```

这是当前最清晰也最完整的一条路径，`llm::LLMStore` 也是构建在这条路径之上。

### 2. 服务端调用链

```text
DBClient / gRPC 请求
  -> MokvServiceImpl
  -> Pod
  -> RaftLog
  -> DB
```

这条路径可以运行，但配置接入和工程化能力还不如嵌入式路径完整。

## 读写流程

### 写入

```text
Put
  -> active MemTable
  -> MemTable 超阈值后进入 flush_queue_
  -> 后台线程构建 SST
  -> Manifest InsertAndUpdate
  -> 达到层级阈值后执行 compaction
```

### 读取

```text
Get
  -> active MemTable
  -> flush_queue_ 中尚未落盘的 MemTable
  -> Manifest 中各层 SST
  -> DataBlock BloomFilter
  -> BlockCache / mmap block
```

## 当前的关键设计点

### DB 层使用 tombstone 表示删除

`DB::Delete()` 并不是直接从 SST 删除，而是写入一个特殊 tombstone 值。读取路径会识别 tombstone 并把它当成“不存在”。

### SST 压缩块是自描述的

DataBlock 现在带有：

- magic
- version
- flags
- compression type
- original entries size

因此读取路径可以在加载时决定是否需要解压，以及应该用哪种压缩器。

### LZ4 已切到标准 frame

LZ4 现在不是仓内自定义格式，而是标准 `liblz4` frame，这一点对外部工具互通很重要。

### LLM 访问层不再手拼原始 key

`mokv/llm/store.hpp` 通过 `Keyspace`、`PromptCacheStore`、`ConversationStore` 等类型管理 key layout 和序列化逻辑。

## 当前需要知道的限制

- server 进程尚未把 `MokvConfig` 作为主配置入口
- `-c` 参数目前没有真正影响 `ConfigManager`
- daemon 模式和配置文件路径的行为还不一致
- `ConversationStore` 的裁剪粒度是“turn 数量”，不是 token 数量
