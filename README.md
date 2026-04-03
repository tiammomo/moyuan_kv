# mokv

`mokv` 是一个面向大模型工作负载的 KV / 元数据存储项目，核心由三层组成：

- `DB`：单进程嵌入式 KV，底层是 `MemTable -> SST -> Manifest` 的 LSM 路径。
- `Raft + gRPC`：提供一个可运行的分布式服务端和最小客户端。
- `llm::LLMStore`：在 KV 之上封装 Prompt Cache、Conversation、Retrieval Metadata、Runtime Config 四类场景接口。

当前代码已经完成以下几件事：

- 目录和构建目标统一为 `mokv`
- `DBKVStore` 支持前缀列举 `ListKeysByPrefix` / `ListEntriesByPrefix`
- SST 压缩读路径可用，默认压缩重新开启
- LZ4 已切换到标准 `liblz4` frame 格式，和外部 `liblz4` / `lz4` CLI 互通
- `ctest` 当前基线为 `11/11` 通过

## 快速开始

### 1. 安装依赖

```bash
sudo bash scripts/install_cpp_env_ubuntu.sh
```

### 2. 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target mokv_server mokv_client -j"$(nproc)"
```

也可以直接用仓库里的便捷脚本：

```bash
./build.sh
```

### 3. 运行测试

```bash
ctest --test-dir build --output-on-failure
```

截至 `2026-04-03`，本地验证结果为 `11/11` 通过。

### 4. 启动服务和客户端

当前服务端默认从当前工作目录读取 `./raft.cfg`，所以请在仓库根目录启动：

```bash
./build/bin/mokv_server
./build/bin/mokv_client
```

客户端当前支持的命令是：

- `put <key> <value>`
- `get <key>`
- `sget <key>`
- `optget <key> <index>`

客户端没有内建 `quit` 命令，结束时直接 `Ctrl-C` 即可。

## 当前能力

### 存储引擎

- `mokv/db.hpp`：统一的 KV 接口，负责 MemTable、刷盘线程、Manifest 和 BlockCache 协调
- `mokv/lsm/skiplist.hpp`：MemTable 底层跳表，当前使用单个 `std::atomic_flag` 自旋锁保护结构
- `mokv/lsm/sst.hpp`：SST 使用 `mmap` 映射文件，DataBlock 带 BloomFilter，可选压缩
- `mokv/lsm/manifest.hpp`：Copy-on-Write Manifest，按层管理 SST 并触发 size-tiered compaction

### 压缩

- 默认压缩配置在 `DBConfig` 中开启
- `CompressionType` 支持 `kNone` / `kSnappy` / `kLZ4`
- `LZ4` 走 `liblz4` 的 frame API
- SST 压缩块使用自描述头，加载时会根据块头自动判断压缩类型和解压方式

### LLM 场景访问层

`mokv/llm/store.hpp` 当前包含：

- `PromptCacheStore`
- `ConversationStore`
- `RetrievalStore`
- `RuntimeConfigStore`
- `LLMStore` facade

Conversation 当前支持的是“按 turn 数裁剪”，对应接口是 `TrimConversationToLastTurns(...)`，不是按 token 数裁剪。

### 服务端与 Raft 路径

- gRPC 服务实现位于 `mokv/raft/service.hpp`
- server 入口位于 `mokv/server.cpp`
- `DBClient` 会按 `raft.cfg` 中的地址列表初始化 RPC client
- `sget` 会尝试走 leader 读

这部分代码目前属于“可运行、可调试”的状态，但仍有几个需要注意的限制：

- `mokv_server -c <file>` 参数已经解析，但当前 `ConfigManager` 仍固定读取 `./raft.cfg`
- daemon 模式会 `chdir("/")`，而配置加载仍依赖相对路径，因此这一路径还不适合作为现成生产部署方案
- `UpdateConfig` RPC 目前直接返回 `CANCELLED`

## 嵌入式用法

### 直接使用 KV 抽象

```cpp
#include "mokv/kvstore.hpp"

int main() {
    mokv::DBKVStore store;
    store.Put("tenant:app:key", "value");

    auto value = store.Get("tenant:app:key");
    auto keys = store.ListKeysByPrefix("tenant:");
    return value.has_value() ? 0 : 1;
}
```

### 使用 LLM 场景访问层

```cpp
#include "mokv/kvstore.hpp"
#include "mokv/llm/store.hpp"

int main() {
    mokv::DBKVStore store;
    mokv::llm::LLMStore llm(store);

    mokv::llm::PromptCacheEntry entry;
    entry.tenant = "tenant-a";
    entry.app_id = "agent-gateway";
    entry.model = "gpt-5.4";
    entry.prompt_hash = "hash-001";
    entry.response = "{\"answer\":\"ok\"}";
    entry.cached_at_ms = 1712345678;

    llm.PutPromptCache(entry);
    auto loaded = llm.GetPromptCache("tenant-a", "agent-gateway", "gpt-5.4", "hash-001");
    return loaded.has_value() ? 0 : 1;
}
```

## 配置入口

这里有两个配置入口，需要区分：

- 嵌入式 / 库内调用：
  - `mokv::DBConfig`
  - `mokv::MokvConfig`
- 当前服务端进程：
  - 实际读取的是仓库根目录下的 `raft.cfg`

也就是说，`MokvConfig` 目前主要用于 `DBKVStore` 这类库内接口；`mokv_server` 还没有把它接成完整的进程配置入口。

## 目录结构

```text
.
├── CMakeLists.txt
├── README.md
├── CLAUDE.md
├── docs/
│   ├── README.md
│   ├── getting-started/
│   ├── design/
│   ├── storage/
│   ├── distributed/
│   ├── llm/
│   ├── operations/
│   ├── project/
│   └── reference/
├── scripts/
│   └── install_cpp_env_ubuntu.sh
├── mokv/
│   ├── db.hpp
│   ├── kvstore.hpp
│   ├── kvstore.cpp
│   ├── llm/
│   │   └── store.hpp
│   ├── lsm/
│   │   ├── memtable.hpp
│   │   ├── skiplist.hpp
│   │   ├── sst.hpp
│   │   ├── manifest.hpp
│   │   └── block_cache.hpp
│   ├── raft/
│   │   ├── config.hpp
│   │   ├── pod.hpp
│   │   ├── raft_log.hpp
│   │   └── service.hpp
│   └── utils/
│       ├── bloom_filter.hpp
│       ├── codec.hpp
│       ├── compression.hpp
│       └── lock.hpp
└── tests/
```

## 文档索引

- [docs/README.md](docs/README.md)
- [docs/getting-started/overview.md](docs/getting-started/overview.md)
- [docs/operations/deployment.md](docs/operations/deployment.md)
- [docs/operations/testing.md](docs/operations/testing.md)
- [docs/llm/applications.md](docs/llm/applications.md)
- [docs/project/agent-memory.md](docs/project/agent-memory.md)

## 当前已验证的测试项

- `skip_list_test`
- `db_test`
- `lock_test`
- `thread_pool_test`
- `list_test`
- `bloom_filter_test`
- `cache_test`
- `compaction_test`
- `cm_sketch_test`
- `compression_test`
- `llm_store_test`
