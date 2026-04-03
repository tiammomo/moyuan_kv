# mokv

面向 **大模型应用场景** 的分布式 KV 与元数据存储系统，底层采用 **Raft + LSM Tree**，上层提供 Prompt Cache、Conversation、RAG Metadata、Runtime Config 等场景化访问接口。

## 快速开始

```bash
# 构建项目
./build.sh

# 运行服务
./build/bin/mokv_server

# 客户端交互
./build/bin/mokv_client
```

## 项目学习

**详细学习文档请查看 [learn_docs/](learn_docs/) 目录**。

### 推荐阅读路径

| 路径 | 内容 | 说明 |
|------|------|------|
| **入门路径** | learn_docs/00 → 01 → 08 | 项目概述 → 架构 → DB 层 |
| **存储引擎** | learn_docs/02 → 03 → 04 → 05 | 跳表 → MemTable → SST → Manifest |
| **分布式系统** | learn_docs/09 | Raft 协议详解 |
| **大模型应用** | learn_docs/14 | Prompt 缓存、对话历史、RAG 等场景 |

完整文档列表见 [learn_docs/README.md](learn_docs/README.md)。

## 特性

- **高性能写入**: LSM Tree 顺序磁盘写入
- **强一致性**: Raft 协议保证分布式一致性
- **并发安全**: 自旋锁保证多线程安全
- **多级缓存**: 布隆过滤器 + Block Cache 加速查询
- **压缩存储**: LZ4/Snappy 压缩
- **异步 I/O**: 基于 io_uring 的高性能写入
- **LLM 场景访问层**: Prompt Cache / Conversation / Retrieval Metadata / Runtime Config
- **前缀扫描能力**: 支持按 keyspace 扫描，方便会话历史、配置列表和 RAG chunk 管理
- **现代化构建**: CMake 构建系统

## 技术栈

| 组件 | 技术 |
|------|------|
| 语言 | C++17 |
| 构建系统 | CMake 3.16+ |
| 网络通信 | gRPC 1.51 |
| 存储引擎 | LSM Tree |
| 分布式协议 | Raft |
| 并发控制 | std::atomic_flag 自旋锁 |
| 异步 I/O | io_uring (Linux) |
| 测试框架 | GoogleTest |

## 大模型应用场景

mokv 适用于大模型应用的多种场景，详见 [learn_docs/14-llm-applications.md](learn_docs/14-llm-applications.md)：

| 场景 | 用途 |
|------|------|
| Prompt Cache | 高频提示词模板缓存，降低 API 调用成本 |
| 对话历史存储 | 多轮对话历史，Raft 保证多节点一致性 |
| RAG 元数据 | 文档 ID、向量 ID 映射，向量库配合使用 |
| API 限流与计数 | 原子性计数，分布式一致性保证 |
| 模型配置管理 | 多模型 API 动态配置，支持热更新 |

## LLM 数据访问层

这次重构后，LLM 场景不再直接依赖裸 `Put/Get` 拼 key，而是通过 [`mokv/llm/store.hpp`](mokv/llm/store.hpp) 中的场景化接口访问：

```cpp
mokv::DBKVStore store;
mokv::llm::LLMStore llm_store(store);

llm_store.PutPromptCache(...);
llm_store.AppendConversationTurn(...);
llm_store.ListConversationSessions(...);
llm_store.PutRetrievalChunk(...);
llm_store.ListRuntimeConfigs(...);
```

## 架构概览

```
┌─────────────────────────────────────────────────────────────┐
│                     mokv 架构                               │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   ┌─────────┐       ┌─────────────────────────────┐       │
│   │ Client  │──────▶│      LLM Access Layer       │       │
│   └─────────┘       │  ┌───────────────────────┐  │       │
│                     │  │ Prompt/Chat/RAG API   │  │       │
│                     │  └───────────────────────┘  │       │
│                     └─────────────┬───────────────┘       │
│                                   │                        │
│                                   ▼                        │
│   ┌─────────────────────────────────────────────────────┐ │
│   │                  Raft Replication Layer             │ │
│   │                 Pod / Service / Client              │ │
│   └───────────────┬────────────────────────────────────┘ │
│                   │                                        │
│                   ▼                                        │
│   ┌─────────────────────────────────────────────────────┐ │
│   │                 LSM Tree Storage Engine             │ │
│   │  ┌──────────┐  ┌──────────┐  ┌──────────────────┐  │ │
│   │  │ MemTable │→ │  SST     │→ │    Manifest      │  │ │
│   │  │ (SkipList)│  │ (mmap)   │  │  - 分层管理      │  │ │
│   │  └──────────┘  └──────────┘  └──────────────────┘  │ │
│   │                                                      │ │
│   │  ┌──────────────────────────────────────────────┐  │ │
│   │  │  性能优化: BlockCache | Compression | IO     │  │ │
│   │  └──────────────────────────────────────────────┘  │ │
│   └─────────────────────────────────────────────────────┘ │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## 项目结构

```
mokv/
├── mokv/                  # 主代码目录
│   ├── llm/                   # 大模型场景访问层
│   │   └── store.hpp          # Prompt/Conversation/RAG/RuntimeConfig
│   ├── lsm/                   # LSM 树存储引擎
│   │   ├── skiplist.hpp       # 并发安全跳表
│   │   ├── memtable.hpp       # 内存表
│   │   ├── sst.hpp            # SST 磁盘文件
│   │   ├── manifest.hpp       # Manifest 元数据管理
│   │   ├── block_cache.hpp    # LRU 块缓存
│   │   └── compression.hpp    # 压缩
│   ├── raft/                  # Raft 协议实现
│   │   ├── pod.hpp            # Raft 节点核心
│   │   ├── raft_log.hpp       # 日志管理
│   │   └── service.hpp        # RPC 服务
│   ├── cache/                 # 缓存模块
│   ├── utils/                 # 工具类
│   │   ├── lock.hpp           # 读写锁
│   │   └── bloom_filter.hpp   # 布隆过滤器
│   ├── db.hpp                 # 核心 DB
│   ├── kvstore.hpp            # 对外 KV 抽象，支持前缀扫描
│   └── server.cpp / client.cpp
├── learn_docs/                # 学习文档目录（必读）
├── tests/                     # 测试文件
├── raft.cfg                   # Raft 集群配置
├── build.sh                   # 构建脚本
└── README.md                  # 本文档
```

## 构建与运行

### 环境要求

```bash
# Ubuntu/Debian
sudo apt install -y build-essential cmake libgrpc++-dev \
    libprotobuf-dev protobuf-compiler-grpc liburing-dev \
    libboost-dev googletest
```

### 构建

```bash
./build.sh
```

### 运行

```bash
# 前台运行
./build/bin/mokv_server

# Daemon 模式
./build/bin/mokv_server -d

# 客户端
./build/bin/mokv_client
```

### 测试

```bash
cd build
ctest --output-on-failure
```

## 文档索引

| 文档 | 说明 |
|------|------|
| [learn_docs/README.md](learn_docs/README.md) | 文档索引和阅读指南 |
| [learn_docs/00-overview.md](learn_docs/00-overview.md) | 项目概述 |
| [learn_docs/01-architecture.md](learn_docs/01-architecture.md) | 架构设计 |
| [learn_docs/02-skiplist.md](learn_docs/02-skiplist.md) | 跳表实现 |
| [learn_docs/03-memtable.md](learn_docs/03-memtable.md) | MemTable |
| [learn_docs/04-sst.md](learn_docs/04-sst.md) | SST 文件 |
| [learn_docs/05-manifest.md](learn_docs/05-manifest.md) | Manifest |
| [learn_docs/06-bloom-filter.md](learn_docs/06-bloom-filter.md) | 布隆过滤器 |
| [learn_docs/07-block-cache.md](learn_docs/07-block-cache.md) | 块缓存 |
| [learn_docs/08-db-layer.md](learn_docs/08-db-layer.md) | DB 层 |
| [learn_docs/09-raft-protocol.md](learn_docs/09-raft-protocol.md) | Raft 协议 |
| [learn_docs/14-llm-applications.md](learn_docs/14-llm-applications.md) | 大模型应用场景 |

## 许可证

MIT License
