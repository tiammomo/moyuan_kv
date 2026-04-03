# 模块关系

## 从高到低的依赖关系

```text
LLMStore
  -> KVStore / DBKVStore
  -> DB
  -> MemTable / Manifest / SST
  -> BloomFilter / BlockCache / Compression

MokvServiceImpl
  -> ResourceManager
  -> Pod
  -> RaftLog
  -> DB
```

## 主要模块

### `DBKVStore`

`DBKVStore` 是上层最直接可用的 KV 抽象，提供：

- `Get`
- `Put`
- `Delete`
- `ListKeys`
- `ListKeysByPrefix`
- `ListEntriesByPrefix`

它还维护了一个保留元数据键：

- `__mokv__:meta:key_index`

用于重建键索引和实现前缀扫描。

### `LLMStore`

`LLMStore` 建立在 `KVStore` 之上，不直接碰底层 `DB`。

当前内部组合的是：

- `PromptCacheStore`
- `ConversationStore`
- `RetrievalStore`
- `RuntimeConfigStore`

这层的关键价值是：

- 统一 keyspace
- 统一文本序列化格式
- 避免上层业务自己手拼 key

### `DB`

`DB` 是真正的存储协调核心，负责：

- active `MemTable`
- `flush_queue_`
- `Manifest`
- `BlockCache`
- 压缩配置透传

### `Pod`

`Pod` 是服务端 Raft 路径的核心协调者，组合：

- `Follower`
- `RaftLog`
- `DB`

### `RaftLog`

`RaftLog` 负责：

- 把 entry 放入内存 ring buffer
- 跟踪 `index_` / `commited_`
- 后台把 committed entry 应用到底层 `DB`

## 关键交互链

### 嵌入式 LLM 元数据链

```text
业务代码
  -> LLMStore
  -> DBKVStore
  -> DB
  -> SST / Manifest
```

### 服务端写入链

```text
DBClient
  -> gRPC Put
  -> MokvServiceImpl::Put
  -> Pod
  -> RaftLog
  -> DB
```

### 服务端读取链

```text
DBClient::Get / SyncGet
  -> gRPC Get
  -> MokvServiceImpl::Get
  -> Pod
  -> DB
  -> MemTable / Manifest / SST
```

## LLM keyspace 与 KV 层的关系

`LLMStore` 的 keyspace 当前大致是：

- `llm:prompt-cache:<tenant>:<app>:<model>:<hash>`
- `llm:conversation:<tenant>:<app>:<session>`
- `llm:conversation:<tenant>:<app>:<session>:meta`
- `llm:conversation:<tenant>:<app>:<session>:turn:<seq>`
- `llm:retrieval:<tenant>:<kb>:<doc>:<chunk>`
- `llm:runtime-config:<tenant>:<app>:<profile>`

这也是为什么 `DBKVStore` 的前缀扫描能力对 LLM 层非常重要。

## 当前边界

从模块关系上看，当前仓库最清晰、最完整的一条路径仍然是：

```text
LLMStore -> DBKVStore -> DB -> LSM
```

而服务端路径：

```text
MokvServiceImpl -> Pod -> RaftLog -> DB
```

虽然可以运行，但还有配置接入和工程化能力待补。
