# 测试说明

本文档只记录当前分支已经存在并实际通过的测试，不再保留旧的失败结论。

## 当前结果

截至 `2026-04-03`，本地执行：

```bash
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

结果为：

- `11/11` 通过
- `0` 失败

## CTest 中的测试目标

| 测试名 | 覆盖内容 |
|------|------|
| `skip_list_test` | 跳表基本行为 |
| `db_test` | DB 写入、读取、重启后恢复 |
| `lock_test` | `RWLock` 行为 |
| `thread_pool_test` | 线程池调度 |
| `list_test` | 链表 / LRU 基础结构 |
| `bloom_filter_test` | BloomFilter 正确性 |
| `cache_test` | BlockCache 行为 |
| `compaction_test` | Manifest / SST compaction |
| `cm_sketch_test` | Count-Min Sketch |
| `compression_test` | 压缩默认开启、SST 压缩读写、LZ4 互通 |
| `llm_store_test` | Prompt Cache、Conversation、Retrieval、Runtime Config |

## 运行方式

### 构建并运行全部测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

### 运行单个测试

```bash
./build/bin/db_test
./build/bin/compression_test
./build/bin/llm_store_test
```

### 使用过滤器运行 GoogleTest

```bash
./build/bin/compression_test --gtest_filter='CompressionInterop.*'
./build/bin/llm_store_test --gtest_filter='LLMStoreTest.ConversationAppendTrimAndList'
```

## 测试隔离方式

`tests/CMakeLists.txt` 当前会为每个测试创建独立工作目录：

- `${binary_dir}/skip_list_test_workdir`
- `${binary_dir}/db_test_workdir`
- ...

这样做的目的是避免这些持久化测试互相污染：

- `manifest`
- `*.sst`
- 压缩和 compaction 产物

## 重点测试说明

### `db_test`

当前覆盖的是两类基础场景：

- `Function`
- `ReadAfterRestart`

它当前是功能回归测试，不是长时间压测。

### `compression_test`

当前压缩测试覆盖：

- `DBConfig` 默认开启压缩
- 压缩 SST 落盘后可重载
- DB 重启后仍能正确读取压缩数据
- 项目输出的 LZ4 frame 可被外部 `liblz4` 解压
- 外部 `liblz4` 生成的 frame 可被项目读取

### `llm_store_test`

当前覆盖四个真实场景：

- Prompt Cache round-trip 和前缀扫描
- Conversation append / list / trim
- Retrieval chunk metadata round-trip
- Runtime Config round-trip

## 当前没有纳入 CTest 的内容

- `tests/benchmark.cpp`

它仍然是一个独立源码文件，但当前没有被加入 `TEST_SOURCES`，所以不会出现在 `ctest` 结果里。

## 建议的回归顺序

当你改动以下模块时，建议优先回归这些测试：

- `mokv/lsm/skiplist.hpp`：
  - `skip_list_test`
  - `db_test`
- `mokv/lsm/sst.hpp`
  - `compression_test`
  - `compaction_test`
  - `db_test`
- `mokv/llm/store.hpp`
  - `llm_store_test`
- `mokv/kvstore.*`
  - `llm_store_test`
  - `db_test`

## 结论

当前测试基线已经从“历史问题记录”变成“可直接回归的通过集”：

- 持久化路径可测
- 压缩路径可测
- LLM 元数据访问层可测
- 所有已纳入 `ctest` 的目标当前都通过
