# MoKV 技术文档

> 本目录包含项目所有技术细节的学习文档，基于 CMake 构建系统。

## 文档分类

### 一、基础入门

| 文件 | 主题 | 说明 |
|------|------|------|
| [00-overview.md](00-overview.md) | 项目概述 | 快速了解 MoKV |
| [01-architecture.md](01-architecture.md) | 架构设计 | 系统分层和组件 |

### 二、存储引擎（LSM Tree）

| 文件 | 主题 | 说明 |
|------|------|------|
| [02-skiplist.md](02-skiplist.md) | 跳表 | O(log n) 有序数据结构 |
| [03-memtable.md](03-memtable.md) | 内存表 | MemTable 封装 |
| [04-sst.md](04-sst.md) | SST 文件 | 磁盘排序字符串表 |
| [05-manifest.md](05-manifest.md) | Manifest | 元数据与分层管理 |
| [11-compression.md](11-compression.md) | 压缩 | LZ4/Snappy 压缩 |

### 三、查询优化

| 文件 | 主题 | 说明 |
|------|------|------|
| [06-bloom-filter.md](06-bloom-filter.md) | 布隆过滤器 | 快速存在性判断 |
| [07-block-cache.md](07-block-cache.md) | 块缓存 | LRU 缓存层 |
| [10-cache-system.md](10-cache-system.md) | 缓存系统 | 完整缓存架构 |

### 四、分布式（Raft）

| 文件 | 主题 | 说明 |
|------|------|------|
| [09-raft-protocol.md](09-raft-protocol.md) | Raft 协议 | 分布式一致性 |

### 五、核心层

| 文件 | 主题 | 说明 |
|------|------|------|
| [08-db-layer.md](08-db-layer.md) | DB 层 | 核心接口设计 |

### 六、工具与模块关系

| 文件 | 主题 | 说明 |
|------|------|------|
| [12-utils.md](12-utils.md) | 工具类 | RWLock、线程池等 |
| [13-module-relationship.md](13-module-relationship.md) | 模块关系 | 组件交互图 |

### 七、大模型应用

| 文件 | 主题 | 说明 |
|------|------|------|
| [14-llm-applications.md](14-llm-applications.md) | LLM 应用场景 | Prompt 缓存、对话历史、RAG 等 |

## 推荐阅读顺序

### 路径 A：存储引擎入门

```
00-overview.md → 01-architecture.md → 02-skiplist.md → 03-memtable.md
```

### 路径 B：完整存储引擎

```
00-overview.md → 01-architecture.md → 02-skiplist.md → 03-memtable.md
                → 04-sst.md → 05-manifest.md → 06-bloom-filter.md
                → 07-block-cache.md → 11-compression.md
```

### 路径 C：分布式系统

```
00-overview.md → 01-architecture.md → 09-raft-protocol.md
```

### 完整学习路径

```
入门 → 架构 → 存储引擎 → 分布式 → 核心层 → 深入理解
```

### 路径 D：大模型应用

```
00-overview.md → 01-architecture.md → 14-llm-applications.md
```

## 核心数据结构

| 数据结构 | 复杂度 | 文档 |
|----------|--------|------|
| SkipList | O(log n) 查找/插入/删除 | 02-skiplist.md |
| MemTable | O(log n) 查找/插入/删除 | 03-memtable.md |
| SST | O(log n) 查找 | 04-sst.md |
| Manifest | O(L×S) 扫描 | 05-manifest.md |
| BloomFilter | O(k) 插入/查询 | 06-bloom-filter.md |
| LRU Cache | O(1) 查找/淘汰 | 07-block-cache.md |

## 技术栈速查

| 类别 | 技术 |
|------|------|
| 编程语言 | C++17 |
| 构建系统 | CMake 3.16+ |
| 网络通信 | gRPC 1.51 + Protobuf |
| 存储引擎 | LSM Tree |
| 分布式协议 | Raft |
| 内存管理 | mmap |
| 压缩算法 | LZ4, Snappy |
| 并发控制 | std::atomic_flag 自旋锁 |
| 测试框架 | GoogleTest |

## 模块索引

### mokv/lsm/

- `skiplist.hpp` - 并发安全跳表
- `memtable.hpp` - 内存表
- `sst.hpp` - SST 文件
- `manifest.hpp` - 元数据管理
- `block_cache.hpp` - 块缓存

### mokv/raft/

- `pod.hpp` - Raft 节点
- `raft_log.hpp` - Raft 日志
- `config.hpp` - 配置

### mokv/utils/

- `lock.hpp` - 读写锁
- `bloom_filter.hpp` - 布隆过滤器
- `global_random.h` - 全局随机数

### mokv/cache/

- `concurrent_cache.hpp` - 并发缓存
- `cm_sketch.hpp` - Count-Min Sketch
