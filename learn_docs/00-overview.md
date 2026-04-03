# MoKV 项目概述

## 项目简介

MoKV 是一个基于 **Raft 协议** 的分布式 KV 存储系统，采用 **LSM Tree** 作为存储引擎。项目使用 C++17 开发，**CMake** 构建，gRPC 进行网络通信。

## 技术栈

| 类别 | 技术选择 |
|------|----------|
| 编程语言 | C++17 |
| 构建工具 | **CMake 3.16+** |
| 网络通信 | gRPC 1.51 + Protocol Buffers |
| 存储引擎 | LSM Tree |
| 分布式协议 | Raft |
| 测试框架 | GoogleTest |
| 异步 I/O | io_uring |

## 项目结构

```
MoKV/
├── CMakeLists.txt              # CMake 根配置
├── mokv/                    # 源代码目录
│   ├── lsm/                    # LSM 树存储引擎
│   │   ├── skiplist.hpp        # 跳表实现
│   │   ├── skiplist_simple.hpp # 简化版跳表
│   │   ├── memtable.hpp        # 内存表
│   │   ├── sst.hpp             # SST 文件
│   │   ├── manifest.hpp        # 元数据管理
│   │   └── block_cache.hpp     # 块缓存
│   ├── raft/                   # Raft 分布式协议
│   │   ├── pod.hpp             # Raft 节点
│   │   ├── raft_log.hpp        # 日志管理
│   │   ├── config.hpp          # 配置
│   │   └── protos/             # Protocol Buffers 定义
│   ├── cache/                  # 缓存模块
│   │   ├── concurrent_cache.hpp # 并发 LRU 缓存
│   │   └── cm_sketch.hpp       # Count-Min Sketch
│   ├── pool/                   # 线程池
│   │   └── thread_pool.hpp
│   ├── utils/                  # 工具类
│   │   ├── lock.hpp            # 读写锁
│   │   ├── bloom_filter.hpp    # 布隆过滤器
│   │   └── global_random.h     # 全局随机数
│   ├── config.hpp              # 统一配置类
│   ├── kvstore.hpp             # KVStore 接口
│   ├── db.hpp                  # 核心数据库接口
│   ├── server.cpp              # 服务器入口
│   └── client.cpp              # 客户端入口
├── tests/                      # 测试文件
│   ├── CMakeLists.txt
│   └── *_test.cpp
└── build/                      # 构建目录
    ├── bin/                    # 可执行文件
    └── lib/                    # 静态库
```

## 核心特性

| 特性 | 说明 |
|------|------|
| 高性能写入 | LSM Tree + SkipList，顺序磁盘写入 |
| 强一致性 | Raft 协议，Leader 选举 + 日志复制 |
| 并发安全 | **std::atomic_flag 自旋锁** + 细粒度锁 |
| 多级缓存 | BlockCache + BloomFilter |
| 压缩存储 | LZ4/Snappy 压缩 |
| 文件映射 | mmap，利用 OS 页缓存 |
| 异步 I/O | io_uring 高效磁盘读写 |
