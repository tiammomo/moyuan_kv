/**
 * @file skiplist.hpp
 * @brief 并发安全跳表实现 - LSM树 MemTable 的内存索引结构
 *
 * 跳表是一种多层链表结构，用于实现 O(log n) 时间复杂度的查找、插入和删除操作。
 * 本实现支持多线程并发读写，使用读写锁（RWLock）保证线程安全。
 *
 * @section skiplist_structure 跳表结构
 * ┌─────────────────────────────────────────────────────────────┐
 * │                      跳表示意图                              │
 * ├─────────────────────────────────────────────────────────────┤
 * │  Level 3:  head ──────────────────────────────→ nullptr     │
 * │  Level 2:  head ───────────→ [A] ────────────→ nullptr     │
 * │  Level 1:  head ──→ [B] ──→ [A] ──→ [C] ────→ nullptr     │
 * │  Level 0:  head ─→ [B] ─→ [A] ─→ [D] ─→ [C] ─→ nullptr     │
 * └─────────────────────────────────────────────────────────────┘
 *
 * 每层都是有序的链表，较高层作为"高速公路"加速查找。
 * 插入时随机决定节点层级（RandLevel），遵循指数分布。
 */

#pragma once
#include <atomic>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mokv/utils/global_random.h"
#include "mokv/utils/lock.hpp"

namespace mokv {
namespace lsm {

/**
 * @brief 跳表节点结构
 *
 * 每个节点包含：
 * - key: 键（使用 std::string 存储，脱离外部生命周期）
 * - value: 值（使用 std::string 存储）
 * - nexts: 多层指针数组，第0层指向后继节点
 */
struct Node {
    Node() = default;

    /**
     * @brief 构造函数
     * @param key 键（会被复制）
     * @param value 值（会被复制）
     * @param size 节点层级数（即 nexts 数组大小）
     */
    Node(std::string_view key, std::string_view value, size_t size)
        : nexts(size, nullptr), key(std::string(key)), value(std::string(value)) {}

    std::vector<Node*> nexts;          ///< 多层指针，nexts[0] 是第0层指针
    std::string key;                   ///< 键（独立存储，不依赖外部内存）
    std::string value;                 ///< 值（独立存储）
};

/**
 * @brief 并发安全跳表类
 *
 * 提供线程安全的键值存储，支持：
 * - Get(key): O(log n) 时间复杂度的查找
 * - Put(key, value): O(log n) 时间复杂度的插入/更新
 * - Delete(key): O(log n) 时间复杂度的删除
 *
 * @section concurrency 并发控制策略
 *
 * 本实现使用读写锁（RWLock）实现一写多读模式：
 * - 写操作（Put/Delete）使用写锁，独占访问
 * - 读操作（Get）使用读锁，允许多个读并发
 *
 * 为避免死锁，采用以下策略：
 * 1. 定位阶段不持有节点锁
 * 2. 写操作按层级顺序获取锁
 * 3. 删除操作使用单独的 delete_rw_lock_
 */
class ConcurrentSkipList {
public:
    /**
     * @brief 跳表迭代器
     *
     * 用于遍历跳表中的所有元素（按 key 升序）
     */
    class Iterator {
    public:
        explicit Iterator(Node* node) : it_(node) {}

        /// @brief 前置递增运算符，移动到下一个节点
        Iterator& operator++() {
            it_ = it_->nexts[0];
            return *this;
        }

        /// @brief 解引用，获取当前节点
        Node& operator*() { return *it_; }

        /// @brief 等于运算符
        bool operator==(const Iterator& rhs) const { return it_ == rhs.it_; }

        /// @brief 不等于运算符
        bool operator!=(const Iterator& rhs) const { return it_ != rhs.it_; }

    private:
        Node* it_;  ///< 当前迭代器指向的节点
    };

    /**
     * @brief 构造函数
     *
     * 创建空跳表，只包含头节点（层级为1）
     */
    ConcurrentSkipList() {
        head_ = new Node();
        head_->nexts.resize(1);
    }

    /// @brief 析构函数，释放所有节点
    ~ConcurrentSkipList() {
        Node* current = head_;
        while (current) {
            Node* next = current->nexts[0];
            delete current;
            current = next;
        }
    }

    // 禁止拷贝
    ConcurrentSkipList(const ConcurrentSkipList&) = delete;
    ConcurrentSkipList& operator=(const ConcurrentSkipList&) = delete;

    /// @brief 返回指向第一个元素的迭代器
    Iterator begin() { return Iterator(head_->nexts[0]); }

    /// @brief 返回指向末尾的迭代器
    Iterator end() { return Iterator(nullptr); }

    /// @brief 获取元素数量
    size_t size() const { return size_.load(std::memory_order_relaxed); }

    /// @brief 获取二进制大小（用于序列化）
    size_t binary_size() const { return binary_size_.load(std::memory_order_relaxed); }

    /**
     * @brief 查找键对应的值
     * @param key 要查找的键
     * @param value 输出参数，找到的值
     * @return 找到返回 true，否则返回 false
     *
     * @section get_algorithm 查找算法
     *
     * 从最高层开始，利用高层链表的快速跳转特性：
     * 1. 从最高层开始
     * 2. 在当前层向后遍历，直到下一个节点键大于目标键
     * 3. 下降到下一层
     * 4. 重复直到第0层
     * 5. 检查第0层的下一个节点是否匹配
     */
    bool Get(std::string_view key, std::string& value) {
        while (lock_.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();  // 让出 CPU
        }

        auto p = head_;
        // 从最高层开始查找
        for (int level = static_cast<int>(head_->nexts.size()) - 1; level >= 0; --level) {
            while (p->nexts[level] && p->nexts[level]->key < key) {
                p = p->nexts[level];
            }
        }

        // 第0层查找
        if (p->nexts[0] && p->nexts[0]->key == key) {
            value = p->nexts[0]->value;
            lock_.clear(std::memory_order_release);
            return true;
        }
        lock_.clear(std::memory_order_release);
        return false;
    }

    /**
     * @brief 插入或更新键值对
     * @param key 键
     * @param value 值
     *
     * @section put_algorithm 插入算法
     *
     * 采用两阶段策略减少锁持有时间：
     * 1. 定位阶段：找到插入位置，不持有节点锁
     * 2. 插入阶段：一次性获取所有需要的锁，然后执行插入
     *
     * 如果 key 已存在，更新其 value。
     * 如果是新 key，随机生成层级后插入。
     */
    void Put(std::string_view key, std::string_view value) {
        while (lock_.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();  // 让出 CPU
        }

        std::vector<Node*> prev_nodes(head_->nexts.size(), head_);
        Node* p = head_;
        for (int level = static_cast<int>(head_->nexts.size()) - 1; level >= 0; --level) {
            while (p->nexts[level] && p->nexts[level]->key < key) {
                p = p->nexts[level];
            }
            prev_nodes[level] = p;
        }

        // ========== 更新已有节点 ==========
        if (prev_nodes[0]->nexts[0] && prev_nodes[0]->nexts[0]->key == key) {
            binary_size_ += value.size() - prev_nodes[0]->nexts[0]->value.size();
            prev_nodes[0]->nexts[0]->value = std::string(value);
            lock_.clear(std::memory_order_release);
            return;
        }

        // ========== 插入新节点 ==========
        auto new_level = RandLevel();
        if (new_level > head_->nexts.size()) {
            const size_t old_levels = head_->nexts.size();
            head_->nexts.resize(new_level, nullptr);
            prev_nodes.resize(new_level, head_);
            for (size_t level = old_levels; level < new_level; ++level) {
                prev_nodes[level] = head_;
            }
        }

        auto node = new Node(std::string(key), std::string(value), new_level);
        ++size_;
        binary_size_ += key.size() + value.size();

        // 执行插入（只更新新节点实际拥有的层级）
        for (size_t level = 0; level < new_level; ++level) {
            node->nexts[level] = prev_nodes[level]->nexts[level];
            prev_nodes[level]->nexts[level] = node;
        }
        lock_.clear(std::memory_order_release);
    }

    /**
     * @brief 删除键值对
     * @param key 要删除的键
     *
     * @section delete_algorithm 删除算法
     *
     * 1. 从最高层开始，找到目标节点的前驱
     * 2. 在每层中移除目标节点的指针
     * 3. 删除节点对象
     * 4. 清理空的高层
     */
    void Delete(std::string_view key) {
        while (lock_.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();  // 让出 CPU
        }
        Node* prev = head_;
        Node* targets[32] = {nullptr};  // 假设最大层级32
        Node* prev_nodes[32] = {nullptr};  // 每层的前驱节点

        // 从最高层开始查找，记录每层的前驱节点
        for (int level = static_cast<int>(head_->nexts.size()) - 1; level >= 0; --level) {
            while (prev->nexts[level] && prev->nexts[level]->key < key) {
                prev = prev->nexts[level];
            }
            prev_nodes[level] = prev;
            if (prev->nexts[level] && prev->nexts[level]->key == key) {
                targets[level] = prev->nexts[level];
            }
        }

        // 从所有层级移除目标节点
        for (int level = static_cast<int>(head_->nexts.size()) - 1; level >= 0; --level) {
            if (targets[level]) {
                prev_nodes[level]->nexts[level] = targets[level]->nexts[level];
            }
        }

        Node* delete_node = targets[0];
        if (!delete_node) {
            lock_.clear(std::memory_order_release);
            return;  // 节点不存在
        }

        --size_;
        binary_size_ -= delete_node->key.size() + delete_node->value.size();
        delete delete_node;

        // 清理空的高层
        size_t new_size = head_->nexts.size();
        while (new_size > 1 && head_->nexts[new_size - 1] == nullptr) {
            --new_size;
        }
        if (new_size < head_->nexts.size()) {
            head_->nexts.resize(new_size);
        }
        lock_.clear(std::memory_order_release);
    }

private:
    /**
     * @brief 随机生成新节点的层级
     *
     * 使用几何分布：每层有 1/4 的概率继续向上
     * 期望层级为 4/3（约1.33），保证 O(log n) 性能
     *
     * @return 新节点的层级（至少为1）
     */
    size_t RandLevel() {
        size_t level = 1;
        while ((cpputil::common::GlobalRand() & 3) == 0) {  // 25% 概率升级
            ++level;
        }
        return level;
    }

private:
    std::atomic_size_t size_{0};               ///< 元素数量
    std::atomic_size_t binary_size_{0};        ///< 二进制大小
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT; ///< 自旋锁，保护所有操作
    Node* head_;                               ///< 头节点（哨兵节点）
};

}  // namespace lsm
}  // namespace mokv
