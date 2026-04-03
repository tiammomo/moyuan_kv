# MoKV 部署指南

本文档记录了在 WSL2 Ubuntu 环境下部署 MoKV 的完整过程。使用 **CMake** 构建。

---

## 系统要求

| 组件 | 最低版本 | 说明 |
|------|---------|------|
| 操作系统 | Ubuntu 24.04 LTS / WSL2 | 已在 Ubuntu 24.04.3 LTS 测试 |
| 内核版本 | Linux 6.x+ | 支持 io_uring |
| 编译器 | GCC 11+ | 需要 C++17 支持 |
| 内存 | 4GB+ | 构建时建议 8GB+ |
| 磁盘空间 | 5GB+ | 依赖和构建产物 |

---

## 安装依赖

```bash
sudo apt update
sudo apt install -y \
    cmake \
    build-essential \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc \
    libgtest-dev \
    googletest \
    liburing-dev \
    libboost-dev
```

### 依赖说明

| 包名 | 用途 |
|------|------|
| cmake | 构建系统 |
| build-essential | GCC/G++ 编译器 |
| libgrpc++-dev | gRPC C++ 开发库 |
| libprotobuf-dev | Protocol Buffers 开发库 |
| protobuf-compiler-grpc | gRPC Protobuf 编译器插件 |
| libgtest-dev | GoogleTest 测试框架 |
| liburing-dev | Linux io_uring 异步 I/O |
| libboost-dev | Boost 并发容器 |

---

## 克隆项目

```bash
git clone https://github.com/tiammomo/MoKV.git
cd MoKV
```

---

## 构建项目

### 方式一：使用构建脚本

```bash
# 构建服务端
chmod +x build.sh
./build.sh

# 构建客户端
chmod +x build_client.sh
./build_client.sh
```

### 方式二：直接使用 CMake

```bash
# Debug 构建（用于测试）
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build . -j$(nproc)

# Release 构建（用于生产）
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

> **注意**: 首次构建需要手动生成 gRPC 代码。如果构建失败，可先运行：
> ```bash
> cd build/mokv/raft/protos
> /usr/bin/protoc --proto_path=/home/ubuntu/learn_projects/MoKV/mokv/raft/protos \
>     --cpp_out=. --grpc_out=. --plugin=protoc-gen-grpc=/usr/bin/grpc_cpp_plugin \
>     raft.proto
> cd /home/ubuntu/learn_projects/MoKV/build
> cmake --build . -j$(nproc)
> ```

构建产物位于：
- `build/bin/mokv_server` - 服务端可执行文件
- `build/bin/mokv_client` - 客户端可执行文件

---

## 运行测试

```bash
cd build
cmake --build . --target all
ctest --output-on-failure --verbose
```

---

## 集群配置

编辑 `raft.cfg` 文件：

```bash
# 单节点测试配置
1
1 127.0.0.1 9001
1 127.0.0.1 9001
```

配置格式说明：
- 第1行：集群节点总数
- 接下来 N 行：各节点配置 (节点ID IP 端口)
- 最后1行：当前本地节点配置 (节点ID IP 端口)

---

## 运行服务

```bash
# 启动服务端
./build/bin/mokv_server

# 在另一个终端启动客户端
./build/bin/mokv_client
```

---

## 客户端操作

```bash
# 写入键值对
put key value

# 读取键值
get key

# 同步读取（线性化读）
sync_get key

# 退出
quit
```

---

## Daemon 模式

### 前台运行

```bash
./build/bin/mokv_server
```

### 后台运行（Daemon）

```bash
# 使用 -d 参数启动为守护进程
./build/bin/mokv_server -d

# 指定配置文件
./build/bin/mokv_server -d -c /path/to/raft.cfg

# 指定日志文件和 PID 文件
./build/bin/mokv_server -d -l /var/log/mokv.log -P /var/run/mokv.pid

# 查看帮助
./build/bin/mokv_server --help
```

### Daemon 模式说明

- 自动创建新会话，脱离终端控制
- 自动重定向标准输入/输出/错误到日志文件
- 自动创建 PID 文件（默认 `mokv.pid`）
- 支持 SIGHUP 信号触发日志轮转
- 支持 SIGTERM/SIGINT 信号优雅关闭

### 管理命令

```bash
# 查看日志
tail -f mokv.log

# 停止服务
kill $(cat mokv.pid)

# 检查进程状态
ps aux | grep mokv
```

---

## 性能基准测试

> **注意**: benchmark 目标当前存在编译错误（与 Raft 日志相关），暂不可用。

### 预期性能指标

| 组件 | 预期性能 |
|------|----------|
| SkipList 插入 | ~500,000+ ops/sec |
| SkipList 读取 | ~1,000,000+ ops/sec |
| 布隆过滤器 | ~10,000,000+ ops/sec |
| Block Cache | 亚毫秒级延迟 |

---

## 多节点集群部署

### 3节点集群配置示例

每个节点的 `raft.cfg` 文件需要包含完整的集群信息：

**节点1 (raft.cfg):**
```bash
3
1 192.168.1.101 9001
2 192.168.1.102 9001
3 192.168.1.103 9001
1 192.168.1.101 9001
```

**节点2 (raft.cfg):**
```bash
3
1 192.168.1.101 9001
2 192.168.1.102 9001
3 192.168.1.103 9001
2 192.168.1.102 9001
```

**节点3 (raft.cfg):**
```bash
3
1 192.168.1.101 9001
2 192.168.1.102 9001
3 192.168.1.103 9001
3 192.168.1.103 9001
```

---

## 部署过程记录

### 环境检查

```bash
# 检查系统版本
cat /etc/os-release
# PRETTY_NAME="Ubuntu 24.04.3 LTS"

# 检查内核版本
uname -r
# 6.6.87.2-microsoft-standard-WSL2
```

### 项目构建（CMake）

```bash
cd /home/ubuntu/learn_projects/MoKV

# 安装依赖
sudo apt update
sudo apt install -y cmake build-essential libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc

# 配置和构建
mkdir -p build && cd build
cmake ..
cmake --build . --target mokv_server mokv_client -j$(nproc)
```

### 构建产物

```bash
ls -la build/bin/
# -rwxr-xr-x 1 ubuntu ubuntu 52934816 mokv_client
# -rwxr-xr-x 1 ubuntu ubuntu 55085632 mokv_server
```

### 运行测试

```bash
# 配置单节点
cat > raft.cfg << 'EOF'
1
1 127.0.0.1 9001
1 127.0.0.1 9001
EOF

# 启动服务
./build/bin/mokv_server &
# server listening on 9001

# 服务启动后会进行 Raft 选举
# select time out flag 0
# start request vote, term 1
# ...
```

---

## 代码修复说明

在构建过程中发现并修复了以下编译问题：

### 历史修复（已完成）

1. **bloom_filter.hpp**: 修复 include 路径 (`SHUAI-KV` → `mokv`)
2. **pod.hpp**: 添加缺失的成员变量 (`election_cv_`, `election_thread_`, `election_thread_mutex_`)
3. **block_cache.hpp**: 修复 C++17 默认成员初始化器问题
4. **sst.hpp**: 添加命名空间前缀、`operator bool()` 方法，修复 `PrefetchDataBlock` 方法
5. **manifest.hpp**: 修复函数调用方式 (`max_level_size_` → `max_level_size()`)，添加 const 版本的 `ssts()` 方法
6. **db.hpp**: 添加 `mutable` 关键字以支持 const 方法中的锁操作

### 最近修复（2024-01）

#### skiplist.hpp 段错误问题

**问题描述**: skip_list_test 测试在 Function 测试阶段发生段错误。

**根本原因**: `GlobalRand()` 函数使用 `std::chrono::high_resolution_clock::now()` 导致不稳定行为。

**修复方案**:
- [global_random.cpp](mokv/utils/global_random.cpp): 简化为仅使用原子递增计数器
- [skiplist.hpp](mokv/lsm/skiplist.hpp): 修复层级扩展逻辑，确保路径上所有节点的 nexts 向量正确扩展

#### 测试验证结果

| 测试 | 状态 | 说明 |
|------|------|------|
| lock_test | PASS | |
| thread_pool_test | PASS | |
| list_test | PASS | |
| bloom_filter_test | PASS | |
| cache_test | PASS | |
| compaction_test | PASS | |
| cm_sketch_test | PASS | |
| skip_list_test Function | PASS | |
| skip_list_test Concurrent | 有时失败 | 竞态条件问题 |

#### 链接顺序问题

**问题描述**: CMake 静态库链接顺序导致符号解析问题。

**修复方案**: 在 tests/CMakeLists.txt 中调整链接顺序，确保 `mokv` 库放在最后：

```cmake
target_link_libraries(${test_name}
    PRIVATE
        GTest::gtest_main
        mokv_grpc
        gRPC::grpc++
        grpc
        protobuf::libprotobuf
        mokv  # 放在最后
)
```

---

## 测试验证

```bash
cd build

# 配置测试
cmake .. -DBUILD_TESTING=ON

# 构建
cmake --build . -j$(nproc)

# 运行所有测试
ctest --output-on-failure

# 或运行单个测试
./bin/skip_list_test
./bin/lock_test
./bin/thread_pool_test
./bin/bloom_filter_test
./bin/cache_test
./bin/compaction_test
./bin/cm_sketch_test
```

---

## 常见问题

### Q1: CMake 找不到 gRPC

```bash
# 确保安装了 gRPC 开发包
sudo apt install -y libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc
```

### Q2: 构建内存不足

```bash
# 限制并行任务数
cmake --build . -- -j 2
```

### Q3: gRPC 连接问题

确保防火墙允许相应端口：
```bash
sudo ufw allow 9001/tcp
```

### Q4: WSL2 网络问题

如果使用 WSL2，确保网络配置正确：
```bash
# 检查 WSL2 IP
ip addr show eth0

# 从 Windows 访问 WSL2 服务
# 使用 localhost 或 WSL2 IP
```

---

## 目录结构

```
MoKV/
├── CMakeLists.txt           # CMake 根配置
│
├── build/                    # CMake 构建目录
│   ├── bin/
│   │   ├── mokv_server
│   │   ├── mokv_client
│   │   └── benchmark
│   └── lib/
│
├── mokv/                  # 源代码目录
│   ├── CMakeLists.txt        # 子模块 CMake 配置
│   ├── config.hpp            # 统一配置类
│   ├── kvstore.hpp           # KVStore 接口
│   ├── lsm/                  # LSM Tree 存储引擎
│   │   ├── skiplist.hpp       # 并发安全跳表
│   │   ├── sst.hpp            # SST 文件
│   │   ├── block_cache.hpp    # LRU 块缓存
│   │   └── ...
│   ├── raft/                 # Raft 协议实现
│   │   ├── pod.hpp            # Raft 节点核心
│   │   ├── raft_log.hpp       # 日志管理
│   │   └── protos/            # protobuf 定义
│   ├── cache/                # 缓存模块
│   ├── utils/                # 工具类
│   │   ├── bloom_filter.hpp   # 布隆过滤器
│   │   └── ring_buffer_queue.hpp
│   ├── server.cpp            # 服务端入口
│   └── client.cpp            # 客户端入口
│
├── tests/                    # 测试
│   ├── CMakeLists.txt        # 测试 CMake 配置
│   ├── *_test.cpp            # 单元测试
│   └── benchmark.cpp         # 性能基准
│
├── raft.cfg                  # Raft 集群配置
├── build.sh                  # 构建脚本
├── build_client.sh           # 客户端构建脚本
├── DEPLOYMENT.md             # 本文档
└── Dockerfile                # Docker 构建
```

---

## 相关文档

- [README.md](README.md) - 项目概述和技术架构
