# Raft 分布式协议

**文件**: `raft/pod.hpp`, `raft/raft_log.hpp`, `raft/follower.hpp`

Raft 是一种共识算法，用于实现分布式一致性。

## Raft 概述

Raft 将共识问题分解为三个独立的子问题：
1. **Leader 选举**: 选出一个 Leader 处理客户端请求
2. **日志复制**: Leader 复制日志到所有 Follower
3. **安全性**: 确保状态机的一致性

## 核心组件

| 组件 | 文件 | 职责 |
|------|------|------|
| Pod | `pod.hpp` | Raft 节点核心实现 |
| RaftLog | `raft_log.hpp` | 日志条目管理 |
| Follower | `pod.hpp` | Follower 节点管理 |
| Service | `service.hpp` | gRPC 服务实现 |

## Pod 节点状态

```cpp
enum class PodStatus {
    Candidate,  // 候选者：正在竞选 Leader
    Leader,     // 领导者：处理客户端请求
    Follower,   // 跟随者：响应 Leader 和候选者
};
```

## Pod 核心字段

```cpp
class Pod {
private:
    std::mutex election_mutex_;                    // 选举互斥锁
    bool election_thread_stop_flag_ = false;       // 选举线程停止标志
    std::atomic_uint64_t last_time_;               // 上次收到心跳时间
    int32_t id_;                                   // 节点 ID
    int32_t leader_id_;                            // 当前 Leader ID
    int32_t voted_ = false;                        // 是否已投票
    PodStatus status_;                             // 节点状态
    std::atomic_int64_t term_{0};                  // 当前任期
    std::vector<std::shared_ptr<Follower>> followers_; // Follower 列表
    RaftLog raft_log_;                             // Raft 日志
    std::shared_ptr<easykv::DB> db_;               // 数据库实例
};
```

## 超时配置

```cpp
class Pod {
public:
    static constexpr int heart_beat_time_ms() { return 1000; }   // 心跳间隔 1秒
    static constexpr int timeout_time_ms() { return 5000; }      // 选举超时 5秒
};
```

## 选举流程

### 状态转换图

```
                        ┌─────────────┐
                        │   Follower  │ ←─────────────────────┐
                        │ (等待心跳)   │                       │
                        └──────┬──────┘                       │
                               │ 超时                         │ 收到 Leader 消息
                               ↓                              │
                        ┌─────────────┐                       │
             赢得选举 → │  Candidate  │ ────→ 收到更高任期 ────┘
                        │ (竞选 Leader)│
                        └──────┬──────┘
                               │ 收到多数投票
                               ↓
                        ┌─────────────┐
                        │    Leader   │
                        │ (处理请求)   │
                        └─────────────┘
```

### RequestVote RPC

```cpp
// 请求结构
struct RequestVoteReq {
    int32_t id;           // 候选者 ID
    int64_t term;         // 候选者任期
    size_t index;         // 候选者最后日志索引
};

// 响应结构
struct RequestVoteRsp {
    int64_t term;         // 响应者任期
    int32_t code;         // 0: 成功, -1: 失败
};
```

### 选举实现

```cpp
bool RequestVote() {
    // 1. 增加任期
    ++term_;

    // 2. 投自己一票
    size_t ticket_num = 1;
    voted_ = true;

    // 3. 向所有 Follower 发送投票请求
    for (auto& follower : followers_) {
        RequestVoteReq req;
        req.set_id(id_);
        req.set_term(term_);
        req.set_index(raft_log_.index());

        RequestVoteRsp rsp;
        auto status = follower->rpc_client().stub().RequestVote(&ctx, req, &rsp);

        if (status.ok() && rsp.base().code() == 0) {
            ++ticket_num;  // 获得投票
        }
    }

    // 4. 检查是否获得多数票
    size_t quorum = (1 + followers_.size()) / 2;
    if (ticket_num > quorum) {
        // 赢得选举，成为 Leader
        status_ = PodStatus::Leader;
        leader_id_ = id_;

        // 启动心跳和日志复制
        for (auto& follower : followers_) {
            follower->Run();
        }
        return true;
    }

    // 选举失败
    status_ = PodStatus::Follower;
    return false;
}
```

## 日志复制

### Entry 结构

```protobuf
message Entry {
    int32 term = 1;       // 条目所属任期
    int32 index = 2;      // 条目索引
    string key = 3;       // 键
    string value = 4;     // 值
    int32 mode = 5;       // 0: put, 1: delete
    int32 commited = 6;   // 是否已提交
}
```

### AppendEntries RPC

```cpp
// 请求结构
struct AppendReq {
    int32_t id;              // Leader ID
    int64_t term;            // Leader 任期
    size_t prev_log_index;   // 前一条日志的索引
    size_t prev_log_term;    // 前一条日志的任期
    repeated Entry entrys;   // 日志条目
    size_t leader_commit;    // Leader 已提交索引
};

// 响应结构
struct AppendRsp {
    int64_t term;       // 响应者任期
    int32_t code;       // 0: 成功, -1: 失败
    size_t last_log_index;  // 响应者最后日志索引
};
```

### Follower 日志复制

```cpp
void Follower::Run() {
    sync_thread_ = std::thread([this]() {
        while (!stop_) {
            if (nextindex_ < main_log_->index()) {
                // 发送 AppendEntries RPC
                AppendReq req;
                req.set_id(leader_id_);
                req.set_term(pod_->Term());
                req.set_prev_log_index(previndex_);
                req.set_prev_log_term(0);  // TODO: 获取任期

                // 填充日志条目
                auto entry = req.add_entrys();
                auto [k, v] = *(main_log_->Get(nextindex_));
                entry->set_key(k);
                entry->set_value(v);

                AppendRsp rsp;
                auto code = rpc_client_.stub().Append(&ctx, req, &rsp);

                if (code.ok()) {
                    if (rsp.base().code() == 0) {
                        // 日志匹配成功
                        ++nextindex_;
                        ++matchindex_;
                    } else {
                        // 日志不匹配，回退重试
                        --nextindex_;
                    }

                    // 检查多数派确认
                    if (CheckQuorum()) {
                        main_log_->UpdateCommit(nextindex_);
                    }
                }
            }
        }
    });
}
```

### 提交确认流程

```
┌─────────────────────────────────────────────────────────────┐
│                    日志复制流程                              │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Leader                                                     │
│     │                                                       │
│     ├───Entry1──→ Follower1 (match=1)                       │
│     │         ──→ Follower2 (match=1)                       │
│     │         ──→ Follower3 (match=0)                       │
│     │                                                       │
│     └───Entry2──→ Follower1 (match=2)                       │
│               ──→ Follower2 (match=2)                       │
│               ──→ Follower3 (match=0)                       │
│                                                             │
│  当 2 个节点确认后（多数派）：                                │
│  - Leader 提交 Entry1, Entry2                               │
│  - 下次心跳通知 Follower 提交                                │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## RaftLog 日志管理

```cpp
class RaftLog {
private:
    constexpr static const char* log_name_ = "raft_log_meta";
    easykv::DB* db_;                               // 底层数据库
    std::mutex lock_;                              // 互斥锁
    std::thread sync_thread;                       // 同步线程
    std::atomic_bool stop_{false};
    std::atomic_size_t index_;                     // 当前日志索引
    cpputil::pbds::RingBufferQueue<Entry> queue_; // 日志条目队列
    size_t commited_;                              // 已提交索引
    size_t last_append_;                           // 最后应用到状态机的索引
    size_t start_index_;                           // 起始索引

public:
    // 添加日志条目
    void Append(const Entry& entry);

    // 获取指定索引的日志
    std::optional<Entry> Get(size_t index);

    // 获取当前索引
    size_t index() const { return index_; }

    // 更新提交索引
    void UpdateCommit(size_t commit_index);

    // 获取已提交的日志并应用
    void Apply(std::shared_ptr<DB> db);
};
```

## RPC 接口定义

```protobuf
service MoKVService {
    // KV 操作
    rpc Put(PutReq) returns (PutRsp) {}
    rpc Get(GetReq) returns (GetRsp) {}

    // 配置更新
    rpc UpdateConfig(Config) returns (UpdateConfigRsp) {}

    // Raft 协议
    rpc RequestVote(RequestVoteReq) returns (RequestVoteRsp) {}
    rpc Append(AppendReq) returns (AppendRsp) {}
}

// KV 操作请求
message PutReq {
    string key = 1;
    string value = 2;
}

message GetReq {
    string key = 1;
}

// KV 操作响应
message PutRsp {
    BaseRsp base = 1;
}

message GetRsp {
    BaseRsp base = 1;
    string value = 2;
}
```

## 安全性保证

### 1. Leader 完整性
如果一个日志条目被提交，它会出现在所有未来 Leader 的日志中。

### 2. 日志匹配
如果两个日志在某个索引处的任期相同，则它们完全相同。

### 3. 只读操作
只读操作由 Leader 处理，可能需要验证自己仍是 Leader：

```cpp
// Leader 处理只读请求时检查
bool IsLeader() {
    // 发送心跳确认自己仍是 Leader
    return CheckHeartbeat();
}
```

## 特性总结

| 特性 | 说明 |
|------|------|
| 强一致性 | 基于 Raft 协议的线性一致性 |
| Leader 选举 | 基于任期和投票机制 |
| 日志复制 | Leader 到 Follower 的异步复制 |
| 故障恢复 | 自动 Leader 选举和日志恢复 |
| 成员变更 | 支持动态配置变更 |
