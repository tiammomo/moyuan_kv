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

仓库默认 `raft.cfg` 是单节点配置：

```text
1
1 127.0.0.1 9001
1 127.0.0.1 9001
```

在仓库根目录启动：

```bash
./build/bin/mokv_server
./build/bin/mokv_client
```

客户端当前支持：

- `put <key> <value>`
- `get <key>`
- `sget <key>`
- `optget <key> <index>`

## `raft.cfg` 格式

当前 `raft::ConfigManager` 读取的格式是：

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

## 运行限制

这部分必须按当前代码来理解：

- `mokv_server -c <file>` 虽然已经解析参数，但当前实现没有把它传给 `raft::ConfigManager`
- `raft::ConfigManager` 仍固定读取当前工作目录下的 `./raft.cfg`
- daemon 模式会 `chdir("/")`，因此如果不额外处理配置文件路径，后台模式下很可能读不到正确的 `raft.cfg`

结论：

- 当前推荐前台运行
- 如果一定要跑 daemon，需要先补代码，把配置路径真正接通；现阶段不建议把 daemon 当成现成生产部署方案

## 日志和 PID

服务端入口支持这些参数：

- `-d, --daemon`
- `-c, --config <file>`
- `-l, --log <file>`
- `-P, --pid <file>`
- `-h, --help`
- `-v, --version`

但请注意，只有 `-d`、`-l`、`-P` 在当前实现里真正影响运行行为；`-c` 还没有接到配置加载路径。

## 测试验证

```bash
ctest --test-dir build --output-on-failure
```

截至 `2026-04-03`，当前分支本地结果为：

- `11/11` 测试通过
- 包含压缩互通测试和 LLM store 测试

## 适合放在哪里

当前仓库更适合这两类使用方式：

- 作为嵌入式 KV / 元数据组件，直接链接 `mokv` 静态库
- 作为实验性或开发环境下的 Raft + gRPC 服务端

如果目标是生产化多节点部署，建议先继续补以下内容：

- 真实生效的服务端配置文件路径
- daemon 模式与配置加载的一致性
- 更完整的运维日志与健康检查
- 更强的 Raft 恢复 / 快照 / 配置变更能力
