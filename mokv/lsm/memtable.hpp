#pragma once
/**
 * @file memtable.hpp
 * @brief 内存表 - LSM树 的活跃数据存储层
 *
 * MemTable 是 LSM 树的内存组件，负责接收所有新写入的数据。
 * 当 MemTable 达到阈值（默认 3MB）时，会触发刷盘（Flush）操作，
 * 将数据持久化到 SST 文件。
 *
 * @section memtable_structure 结构
 * MemTable 内部封装了 ConcurrentSkipList，所有操作最终由跳表完成。
 *
 * @section lifecycle 生命周期
 * 1. 新写入 → MemTable
 * 2. 达到阈值 → 标记为只读，创建新的 MemTable
 * 3. 后台线程 → 将只读 MemTable 刷盘到 SST
 * 4. 刷盘完成 → 释放内存
 */

#include "mokv/lsm/skiplist.hpp"

namespace mokv {
namespace lsm {

/**
 * @class MemTable
 * @brief 内存表，存储活跃键值对数据
 *
 * MemTable 是用户写入的第一站，所有 Put 操作首先写入 MemTable。
 * 它提供与标准 map 相似的接口，但内部使用跳表实现有序存储。
 *
 * @note 线程安全：内部使用 ConcurrentSkipList 保证并发安全
 */
class MemTable {
public:
    using Iterator = ConcurrentSkipList::Iterator;  ///< 迭代器类型

    /**
     * @brief 查询键对应的值
     * @param key 要查询的键
     * @param value 输出：查询到的值
     * @return true 表示找到，false 表示不存在
     */
    bool Get(std::string_view key, std::string& value) {
        return skip_list_.Get(key, value);
    }

    /**
     * @brief 插入或更新键值对
     * @param key 键
     * @param value 值
     *
     * 如果 key 已存在，则更新其 value
     */
    void Put(std::string_view key, std::string_view value) {
        skip_list_.Put(key, value);
    }

    /**
     * @brief 删除键值对
     * @param key 要删除的键
     *
     * 如果 key 不存在，则什么也不做
     */
    void Delete(std::string_view key) {
        skip_list_.Delete(key);
    }

    /**
     * @brief 获取二进制大小
     * @return 键值对的二进制表示大小（字节）
     *
     * 用于判断是否达到刷盘阈值
     */
    size_t binary_size() {
        return skip_list_.binary_size();
    }

    /**
     * @brief 获取元素数量
     * @return 存储的键值对数量
     */
    size_t size() {
        return skip_list_.size();
    }

    /**
     * @brief 获取起始迭代器
     * @return 指向最小键的迭代器
     */
    Iterator begin() {
        return skip_list_.begin();
    }

    /**
     * @brief 获取结束迭代器
     * @return 指向末尾的迭代器
     */
    Iterator end() {
        return skip_list_.end();
    }

private:
    ConcurrentSkipList skip_list_;  ///< 内部跳表，存储所有键值对
};

// 兼容旧名称（拼写错误的历史遗留）
using MemeTable = MemTable;

}
}