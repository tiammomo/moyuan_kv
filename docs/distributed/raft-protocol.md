# Raft 路径

**对应文件**：

- `mokv/raft/config.hpp`
- `mokv/raft/pod.hpp`
- `mokv/raft/raft_log.hpp`
- `mokv/raft/service.hpp`
- `mokv/raft/client.hpp`

## 先说结论

当前仓库里确实有一条可运行的 Raft + gRPC 路径，但它更适合：

- 本地调试
- 演示读写复制流程
- 作为后续工程化演进的基础

它还不等同于一个已经完整产品化的 Raft 服务。

## 当前有哪些组件

### `ConfigManager`

`mokv/raft/config.hpp` 中的 `ConfigManager` 当前负责：

- 接收 `MokvConfig`
- 生成完整节点列表 `Config`
- 记录本地地址 `local_address`

它既可以通过默认路径读取旧 `raft.cfg`，也可以通过 `Load(path)` 加载新的 `key=value` 配置文件。

### `MokvServiceImpl`

`mokv/raft/service.hpp` 暴露的 RPC 入口包括：

- `Put`
- `Get`
- `UpdateConfig`
- `RequestVote`
- `Append`

其中：

- `Put` / `Get` 已接到 `ResourceManager::instance().pod()`
- `RequestVote` / `Append` 已接到 Raft 节点逻辑
- `UpdateConfig` 当前直接返回 `grpc::Status::CANCELLED`

### `Pod`

`Pod` 是节点核心实现，内部维护：

- 当前节点角色 `PodStatus`
- 任期 `term_`
- `followers_`
- `raft_log_`
- 指向底层 `DB` 的共享引用

当前角色枚举仍然是：

```cpp
enum class PodStatus {
    Candidate,
    Leader,
    Follower,
};
```

### `Follower`

当前 `Follower` 是 `pod.hpp` 里的辅助类，负责：

- 保存某个远端地址
- 维护到该地址的 RPC client
- 同步 append / heartbeat

### `RaftLog`

`RaftLog` 目前由两部分组成：

- 内存中的 `RingBufferQueue<Entry>`
- 本地 `raft_log_meta` 文件

当前 `raft_log_meta` 只保存已提交位置等最小元数据，不是完整 WAL 文件。

## 当前写入路径

```text
DBClient.Put
  -> gRPC Put
  -> MokvServiceImpl::Put
  -> Pod::Put
  -> RaftLog::Put
  -> follower 复制
  -> RaftLog::UpdateCommit
  -> RaftLog 后台线程把已提交 entry 应用到 DB
```

## 当前读路径

### 普通读

`DBClient::Get(...)` 会依次尝试节点列表中的客户端。

### leader 读

`DBClient::SyncGet(...)` 会：

1. 发送 `read_from_leader = true`
2. 如果收到 `leader redirect`
3. 再重试 leader 节点

这也是客户端命令 `sget` 的来源。

## 当前配置链路

服务端和客户端现在共享同一套配置模型：

```text
配置文件 / 环境变量
  -> MokvConfig
  -> ConfigManager
  -> Pod / DBClient
```

其中：

- `mokv_server -c <file>` 会把文件加载进 `MokvConfig`
- `mokv_client -c <file>` 会使用同一份节点列表初始化 RPC client
- daemon 模式会在 `chdir("/")` 之前完成配置加载

## 当前实现里已经存在的超时与线程

需要注意的是，实际时间控制分散在多处实现里：

- client RPC deadline：常见是 `2s` 或 `10s`
- `Follower::Run()` 中存在后台同步线程
- `RaftLog` 自己也有后台线程负责把 committed entry 应用到 `DB`

所以这条路径已经是多线程协作实现，而不是单个同步调用链。

## 已知边界

以下内容在当前代码里是明确存在的边界：

- `UpdateConfig` RPC 尚未实现
- snapshot / membership change 还没有打通
- `raft_log_meta` 不是完整持久化日志
- 当前服务端测试只覆盖最小单节点和 redirect 语义，还没有多进程多节点集成回归

## 如何看待这条路径

如果你在读代码，建议把它理解为：

- 有选举和日志复制框架
- 有最小可跑通的服务入口
- 但仍然需要继续工程化

如果你要改它，建议先从这几个文件一起看：

1. `mokv/server.cpp`
2. `mokv/resource_manager.hpp`
3. `mokv/raft/service.hpp`
4. `mokv/raft/pod.hpp`
5. `mokv/raft/raft_log.hpp`
