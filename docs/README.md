# mokv 文档索引

`docs/` 是当前仓库唯一的长期文档目录，按“上手、设计、存储、分布式、LLM、运维、项目记忆、工具参考”拆分。

## 推荐阅读顺序

### 快速建立全局视图

```text
getting-started/overview.md
-> design/system-architecture.md
-> storage/db-layer.md
```

### 深入存储引擎

```text
storage/skiplist.md
-> storage/memtable.md
-> storage/sst.md
-> storage/manifest.md
-> storage/compression.md
```

### 看分布式和服务端路径

```text
distributed/raft-protocol.md
-> design/module-relationships.md
```

### 看大模型场景封装

```text
llm/applications.md
```

## 目录说明

| 子目录 | 作用 | 代表文档 |
|------|------|------|
| [`getting-started/`](getting-started/) | 建立项目全局认知 | [`overview.md`](getting-started/overview.md) |
| [`design/`](design/) | 总体架构和模块边界 | [`system-architecture.md`](design/system-architecture.md) |
| [`storage/`](storage/) | LSM、缓存、压缩、DB 协调 | [`db-layer.md`](storage/db-layer.md) |
| [`distributed/`](distributed/) | Raft 和 gRPC 服务路径 | [`raft-protocol.md`](distributed/raft-protocol.md) |
| [`llm/`](llm/) | Prompt/Conversation/RAG/Config 场景 | [`applications.md`](llm/applications.md) |
| [`operations/`](operations/) | 部署、编译、测试 | [`deployment.md`](operations/deployment.md) |
| [`project/`](project/) | 项目长期记忆、协作约束 | [`agent-memory.md`](project/agent-memory.md) |
| [`reference/`](reference/) | 通用工具和底层辅助组件 | [`utils.md`](reference/utils.md) |

## 文档清单

### Getting Started

- [`getting-started/overview.md`](getting-started/overview.md)：项目定位、目录结构、当前能力边界

### Design

- [`design/system-architecture.md`](design/system-architecture.md)：嵌入式 KV、Raft 服务、LLM 访问层的整体结构
- [`design/module-relationships.md`](design/module-relationships.md)：真实调用链、模块依赖和边界

### Storage

- [`storage/skiplist.md`](storage/skiplist.md)：跳表结构和当前并发模型
- [`storage/memtable.md`](storage/memtable.md)：MemTable 在 DB 数据流中的角色
- [`storage/sst.md`](storage/sst.md)：SST 文件、索引块、自描述压缩块
- [`storage/manifest.md`](storage/manifest.md)：Copy-on-Write Manifest 和 compaction
- [`storage/bloom-filter.md`](storage/bloom-filter.md)：DataBlock BloomFilter
- [`storage/block-cache.md`](storage/block-cache.md)：热块缓存
- [`storage/db-layer.md`](storage/db-layer.md)：DB 层 tombstone、刷盘线程、Manifest 协调
- [`storage/cache-system.md`](storage/cache-system.md)：cache 相关组合实现
- [`storage/compression.md`](storage/compression.md)：Snappy/LZ4 压缩路径和 `liblz4` frame 互通

### Distributed

- [`distributed/raft-protocol.md`](distributed/raft-protocol.md)：当前 gRPC/Raft 实现和限制

### LLM

- [`llm/applications.md`](llm/applications.md)：Prompt Cache、Conversation、Retrieval Metadata、Runtime Config

### Operations

- [`operations/deployment.md`](operations/deployment.md)：依赖安装、构建、启动和部署限制
- [`operations/testing.md`](operations/testing.md)：测试目标、运行方式和当前基线

### Project

- [`project/agent-memory.md`](project/agent-memory.md)：项目长期记忆、约束、默认工作方式

### Reference

- [`reference/utils.md`](reference/utils.md)：`RWLock`、`ThreadPool`、`RingBufferQueue`、`codec`

## 当前基线

截至 `2026-04-03`：

- 仓库主目录名已经统一为 `mokv`
- LZ4 已切到 `liblz4` 标准 frame
- `MokvConfig` 已接通 server、client 和 `ConfigManager`
- `ctest --test-dir build --output-on-failure` 为 `13/13` 通过

## 当前需要注意的边界

- `mokv_server` / `mokv_client` 共享 `MokvConfig -> ConfigManager` 配置链路
- 旧 `raft.cfg` 仍然兼容，但新的 `key=value` 配置文件已经可用
- `ConversationStore` 当前支持的是“按 turn 数裁剪”，不是按 token 数裁剪
- `RetrievalStore` 存储的是检索元数据和 `embedding_ref`，不是向量本体
