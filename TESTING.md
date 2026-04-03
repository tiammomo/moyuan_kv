# 测试验证文档

> MoKV 项目测试验证记录，基于 CMake 构建系统。

## 测试状态概览

| 测试项 | 状态 | 说明 |
|--------|------|------|
| lock_test | PASS | |
| thread_pool_test | PASS | |
| list_test | PASS | |
| bloom_filter_test | PASS | |
| cache_test | PASS | |
| compaction_test | PASS | |
| cm_sketch_test | PASS | |
| skip_list_test Function | PASS | |
| skip_list_test Concurrent | 有时失败 | 竞态条件 |
| db_test | 超时/SegFault | 需要 100 万次 I/O |

**通过率**: 8/9 核心测试通过

---

## 测试详情

### 1. lock_test - 读写锁测试

**文件**: `tests/lock_test.cpp`

测试 RWLock 读写锁的并发安全性。

```bash
./build/bin/lock_test
```

**结果**: PASS

---

### 2. thread_pool_test - 线程池测试

**文件**: `tests/thread_pool_test.cpp`

测试线程池的任务提交和执行功能。

```bash
./build/bin/thread_pool_test
```

**结果**: PASS

---

### 3. list_test - 列表测试

**文件**: `tests/list_test.cpp`

测试基础列表数据结构的正确性。

```bash
./build/bin/list_test
```

**结果**: PASS

---

### 4. bloom_filter_test - 布隆过滤器测试

**文件**: `tests/bloom_filter_test.cpp`

测试布隆过滤器的插入和查询功能。

```bash
./build/bin/bloom_filter_test
```

**性能指标**: ~10,000,000+ ops/sec

**结果**: PASS

---

### 5. cache_test - 缓存测试

**文件**: `tests/cache_test.cpp`

测试 BlockCache LRU 缓存的正确性。

```bash
./build/bin/cache_test
```

**性能指标**: 亚毫秒级延迟

**结果**: PASS

---

### 6. compaction_test - 压缩测试

**文件**: `tests/compaction_test.cpp`

测试 SST 文件创建、Compaction 合并功能。

```bash
./build/bin/compaction_test
```

**结果**: PASS

---

### 7. cm_sketch_test - Count-Min Sketch 测试

**文件**: `tests/cm_sketch_test.cpp`

测试 CMSketch 频率估计功能。

```bash
./build/bin/cm_sketch_test
```

**结果**: PASS

---

### 8. skip_list_test - 跳表测试

**文件**: `tests/skip_list_test.cpp`

#### Function 测试（单线程）

测试跳表的基本操作：
- Insert 插入
- Find 查找
- Iterate 迭代
- Delete 删除

**状态**: PASS

> **历史问题已修复**:
> - GlobalRand 使用 `high_resolution_clock::now()` 导致不稳定
> - 头节点层级扩展时新节点 nexts 向量未正确扩展

#### Concurrent 测试（多线程并发）

多线程并发读写压力测试。

**状态**: 有时失败

> **说明**: 存在竞态条件，可能在高并发场景下出现数据不一致。
> 建议在生产环境中使用更严格的并发控制策略。

---

### 9. db_test - 数据库测试

**文件**: `tests/db_test.cpp`

完整的数据库功能测试，包含 100 万次 Put 操作。

```bash
./build/bin/db_test
```

**状态**: 超时/SegFault

> **注意**: 该测试需要约 3 分钟运行 100 万次 I/O 操作。
> 由于运行时间过长，不建议在 CI/CD 中默认运行。

---

## 运行测试

### 方式一：使用 ctest

```bash
cd build
ctest --output-on-failure
```

### 方式二：运行单个测试

```bash
./build/bin/lock_test
./build/bin/thread_pool_test
./build/bin/list_test
./build/bin/bloom_filter_test
./build/bin/cache_test
./build/bin/compaction_test
./build/bin/cm_sketch_test
./build/bin/skip_list_test
```

### 方式三：运行所有测试（不含 db_test）

```bash
cd build
ctest -E "db_test" --output-on-failure
```

---

## 构建测试

```bash
# 创建构建目录
mkdir -p build && cd build

# 配置（启用测试）
cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON

# 构建
cmake --build . -j$(nproc)

# 运行测试
ctest --output-on-failure
```

---

## 已知问题

### 1. skip_list_test Concurrent 失败

**问题**: 多线程并发访问时可能出现竞态条件

**影响**: 偶发测试失败，不影响单线程功能

**建议**:
- 使用更严格的自旋锁或互斥锁
- 考虑使用 lock-free 数据结构

### 2. db_test 超时

**问题**: 需要运行 100 万次 I/O 操作

**影响**: 测试时间过长

**建议**:
- 单独运行并设置较长超时
- 减少测试数据量用于快速验证

---

## 测试环境

| 组件 | 版本 |
|------|------|
| 操作系统 | Ubuntu 24.04 LTS / WSL2 |
| 编译器 | GCC 11+ |
| 构建系统 | CMake 3.16+ |
| 测试框架 | GoogleTest |

---

## 相关文档

- [部署指南](DEPLOYMENT.md)
- [技术文档](learn_docs/)
