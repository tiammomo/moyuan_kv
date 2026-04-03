# 项目概览

## 项目定位

`mokv` 当前是一个“存储内核 + 服务端入口 + LLM 场景封装”三合一仓库：

- 底层：LSM Tree KV
- 中层：gRPC + Raft 路径
- 上层：面向大模型应用的元数据访问层

它既可以：

- 作为嵌入式库直接链接 `mokv` 静态库
- 也可以启动 `mokv_server` / `mokv_client` 走现有的服务端路径

## 当前代码里已经稳定存在的能力

- `DB`：提供 Put / Get / Delete / Exists
- `DBKVStore`：补了前缀扫描和键索引管理
- `llm::LLMStore`：
  - Prompt Cache
  - Conversation
  - Retrieval Metadata
  - Runtime Config
- 压缩：
  - 默认开启
  - LZ4 使用标准 `liblz4` frame
  - SST 压缩读路径已打通

## 当前架构的现实边界

需要按当前代码理解，而不是按历史设计稿理解：

- `mokv_server` 的进程级配置入口目前主要还是 `raft.cfg`
- `MokvConfig` 没有被完整接入 server 启动路径
- daemon 选项存在，但和配置加载路径还没有完全打通
- `UpdateConfig` RPC 还没有完成

## 代码目录

```text
mokv/
├── mokv/
│   ├── db.hpp
│   ├── kvstore.hpp
│   ├── kvstore.cpp
│   ├── llm/store.hpp
│   ├── lsm/
│   ├── raft/
│   ├── cache/
│   ├── pool/
│   └── utils/
├── docs/
│   ├── getting-started/
│   ├── design/
│   ├── storage/
│   ├── distributed/
│   ├── llm/
│   ├── operations/
│   ├── project/
│   └── reference/
├── tests/
├── scripts/
├── README.md
└── CLAUDE.md
```

## 技术栈

| 类别 | 当前实现 |
|------|------|
| 语言 | C++17 |
| 构建 | CMake 3.16+ |
| RPC | gRPC + Protobuf |
| 存储引擎 | LSM Tree |
| 一致性 | Raft |
| SST 访问 | `mmap` |
| 压缩 | `liblz4` frame + 内嵌 Snappy 实现 |
| 测试 | GoogleTest |

## 适合拿它做什么

当前最适合的两类用途是：

1. 在本地或服务进程中嵌入一个支持持久化、压缩和前缀扫描的 KV / 元数据层
2. 为大模型应用搭一个简单的 Prompt / Chat / RAG Metadata / Config 存储层

## 测试基线

截至 `2026-04-03`，本地 `ctest` 结果为 `11/11` 通过。
