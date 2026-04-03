# SkipList

**对应文件**：`mokv/lsm/skiplist.hpp`

## 当前实现是什么

`ConcurrentSkipList` 是 `MemTable` 的底层有序结构，负责：

- `Get`
- `Put`
- `Delete`
- 有序遍历

当前实现和旧版文档里最大的区别是：

- 它现在不是节点级 `RWLock` 模型
- 而是用一个全局 `std::atomic_flag lock_` 保护整棵跳表

所以它的并发语义更准确地说是：

- 结构级串行写
- 读写都走同一把自旋锁

## 关键字段

```cpp
class ConcurrentSkipList {
private:
    std::atomic_size_t size_{0};
    std::atomic_size_t binary_size_{0};
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    Node* head_;
};
```

`Node` 结构也比较简单：

```cpp
struct Node {
    std::vector<Node*> nexts;
    std::string key;
    std::string value;
};
```

当前节点本身没有单独的锁。

## 查找

查找流程仍然是标准跳表路径：

1. 从最高层开始
2. 在当前层持续向右移动，直到下一个 key 不再小于目标 key
3. 下降一层继续
4. 最终在第 0 层判断是否命中

实现上，`Get()` 进入时先获取 `lock_`，结束时释放。

## 插入 / 更新

`Put()` 当前分两种情况：

- key 已存在：直接覆盖 value，并更新 `binary_size_`
- key 不存在：先记录各层前驱节点，再按随机层级插入新节点

本轮重构里修掉了一个真实 bug：

- 旧实现里高层插入时使用了错误的前驱节点
- 现在会先构造 `prev_nodes`
- 再按每层自己的前驱完成链接

这也是为什么当前 `skip_list_test` 已经能稳定通过。

## 删除

删除流程是：

1. 记录每层前驱节点 `prev_nodes`
2. 记录目标节点 `targets`
3. 从高层到低层把目标节点摘掉
4. 删除节点对象
5. 回收 head 空出来的高层

当前实现里 `targets` / `prev_nodes` 是固定大小数组，最大按 `32` 层处理。

## 随机层级

随机层级生成逻辑依赖 `cpputil::common::GlobalRand()`：

```cpp
size_t RandLevel() {
    size_t level = 1;
    while ((cpputil::common::GlobalRand() & 3) == 0) {
        ++level;
    }
    return level;
}
```

`GlobalRand()` 现在是原子递增计数器，不再依赖时钟。

## 复杂度

理论平均复杂度仍然是：

| 操作 | 平均复杂度 |
|------|------|
| `Get` | `O(log n)` |
| `Put` | `O(log n)` |
| `Delete` | `O(log n)` |

但要注意，当前并发模型是“整棵跳表一把自旋锁”，所以高并发下的实际扩展性会受到锁竞争影响。

## 当前测试覆盖

- `skip_list_test`
- 间接通过 `db_test`、`compaction_test` 再覆盖一轮

截至 `2026-04-03`，这些测试都已经通过。
