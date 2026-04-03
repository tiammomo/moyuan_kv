# mokv 部署与运行说明

本文档只描述当前代码已经落地并验证过的部署路径，不再保留旧仓库名、旧脚本和历史构建流程。

## 适用环境

- Ubuntu 24.04 / WSL2 Ubuntu 24.04
- GCC 11+ 或 Clang 14+
- CMake 3.16+
- Linux 文件系统，支持 `mmap`

## 依赖安装

### 推荐方式

```bash
sudo bash scripts/install_cpp_env_ubuntu.sh
```

这个脚本会安装：

- `build-essential`
- `cmake`
- `ninja-build`
- `pkg-config`
- `clang` / `clangd` / `lldb`
- `gdb`
- `libgrpc++-dev`
- `libprotobuf-dev`
- `protobuf-compiler`
- `protobuf-compiler-grpc`
- `liburing-dev`
- `libboost-all-dev`
- `liblz4-dev`
- `googletest`

### 手动安装

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  clang \
  clangd \
  gdb \
  lldb \
  libgrpc++-dev \
  libprotobuf-dev \
  protobuf-compiler \
  protobuf-compiler-grpc \
  liburing-dev \
  libboost-all-dev \
  liblz4-dev \
  googletest
```

`liblz4-dev` 是当前分支必需依赖，因为 `mokv/utils/compression.hpp` 已经切到标准 `liblz4` frame API。

## 获取代码

当前仓库远端已经迁移，建议使用新的仓库地址：

```bash
git clone https://github.com/tiammomo/moyuan_kv.git
cd moyuan_kv
```

如果你已经在本地用的是目录名 `mokv`，保持现状即可，和代码本身没有冲突。

## 构建

### 方式一：直接用 CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target mokv_server mokv_client -j"$(nproc)"
```

### 方式二：使用便捷脚本

```bash
./build.sh
```

构建产物位于：

- `build/bin/mokv_server`
- `build/bin/mokv_client`

说明：

- 根 `CMakeLists.txt` 会显式查找 `gRPC`、`Protobuf` 和 `liblz4`
- `mokv/CMakeLists.txt` 会在 `mokv/raft/protos/` 中缺少生成文件时尝试自动调用 `protoc`

## 单节点运行

仓库兼容两种单节点配置格式。

### 方式一：旧 `raft.cfg`

```text
1
1 127.0.0.1 9001
1 127.0.0.1 9001
```

用兼容格式启动：

```bash
./build/bin/mokv_server -c ./raft.cfg
./build/bin/mokv_client -c ./raft.cfg
```

客户端当前支持：

- `put <key> <value>`
- `get <key>`
- `sget <key>`
- `optget <key> <index>`

### 方式二：新的 `mokv.conf`

```text
host=127.0.0.1
port=9001
data_dir=./data
max_memory_mb=4096
block_cache_size_mb=256
memtable_size_mb=64
node_id=1
election_timeout_ms=5000
heartbeat_interval_ms=1000
snapshot_interval_s=3600
enable_compaction=true
level0_compaction_threshold=4
background_threads=4
max_background_jobs=8
verbose_logging=false
log_level=INFO
peer=1,127.0.0.1,9001
```

用新格式启动：

```bash
./build/bin/mokv_server -c ./mokv.conf
./build/bin/mokv_client -c ./mokv.conf
```

## 配置格式

### 旧 `raft.cfg` 格式

`MokvConfig::LoadFromFile(...)` 兼容历史 `raft.cfg`，格式是：

1. 第一行：节点总数
2. 接下来 `N` 行：集群内所有节点的 `id ip port`
3. 最后一行：本地节点的 `id ip port`

三节点示例：

### 节点 1

```text
3
1 192.168.1.101 9001
2 192.168.1.102 9001
3 192.168.1.103 9001
1 192.168.1.101 9001
```

### 节点 2

```text
3
1 192.168.1.101 9001
2 192.168.1.102 9001
3 192.168.1.103 9001
2 192.168.1.102 9001
```

### 节点 3

```text
3
1 192.168.1.101 9001
2 192.168.1.102 9001
3 192.168.1.103 9001
3 192.168.1.103 9001
```

### 新 `key=value` 格式

新的配置文件是逐行 `key=value`，并允许重复 `peer=`：

```text
host=192.168.1.101
port=9001
data_dir=/var/lib/mokv/node1
node_id=1
peer=1,192.168.1.101,9001
peer=2,192.168.1.102,9001
peer=3,192.168.1.103,9001
```

### 环境变量覆盖

当前支持的环境变量包括：

- `MOKV_HOST`
- `MOKV_PORT`
- `MOKV_DATA_DIR`
- `MOKV_MAX_MEMORY`
- `MOKV_BLOCK_CACHE_MB`
- `MOKV_MEMTABLE_MB`
- `MOKV_NODE_ID`
- `MOKV_ELECTION_TIMEOUT_MS`
- `MOKV_HEARTBEAT_INTERVAL_MS`
- `MOKV_SNAPSHOT_INTERVAL_S`
- `MOKV_ENABLE_COMPACTION`
- `MOKV_VERBOSE_LOGGING`
- `MOKV_LOG_LEVEL`
- `MOKV_PEERS`

其中 `MOKV_PEERS` 格式是 `id,host,port;id,host,port;...`。

## 运行限制

这部分必须按当前代码来理解：

- `mokv_server` / `mokv_client` 现在共享 `MokvConfig -> ConfigManager` 配置链路
- `-c, --config <file>` 当前已经真实生效
- daemon 模式仍会 `chdir("/")`，但配置文件会在 daemonize 前读取，不再受相对路径影响
- `data_dir` 当前会承载 `manifest`、`*.sst` 和 `raft_log_meta`
- `UpdateConfig` RPC 仍未实现，集群成员变更和快照恢复也还不完整

结论：

- 当前推荐前台运行，方便观察日志和调试 Raft 行为
- daemon 模式现在可用，但仍不建议把它当成现成生产部署方案

## 日志和 PID

服务端入口支持这些参数：

- `-d, --daemon`
- `-c, --config <file>`
- `-l, --log <file>`
- `-P, --pid <file>`
- `-h, --help`
- `-v, --version`

其中 `-c`、`-d`、`-l`、`-P` 当前都已经真正影响运行行为。

## 测试验证

```bash
ctest --test-dir build --output-on-failure
```

截至 `2026-04-03`，当前分支本地结果为：

- `13/13` 测试通过
- 包含压缩互通测试、RPC 服务语义测试和配置链路测试

## 适合放在哪里

当前仓库更适合这两类使用方式：

- 作为嵌入式 KV / 元数据组件，直接链接 `mokv` 静态库
- 作为实验性或开发环境下的 Raft + gRPC 服务端

如果目标是生产化多节点部署，建议先继续补以下内容：

- 更完整的运维日志与健康检查
- 更强的 Raft 恢复 / 快照 / 配置变更能力
