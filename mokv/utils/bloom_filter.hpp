/**
 * @file bloom_filter.hpp
 * @brief 布隆过滤器实现 - 概率性数据结构
 *
 * 布隆过滤器是一种空间效率极高的概率性数据结构，用于判断元素是否可能存在。
 * 它有以下特性：
 * - 空间效率高：使用位数组存储
 * - 查询速度快：O(k) 时间复杂度，k 为哈希函数数量
 * - 可能存在假阳性（false positive）：可能误判存在，不会漏判
 * - 不支持删除：删除操作会导致假阴性
 *
 * @section bloom_filter_structure 布隆过滤器结构
 * ┌─────────────────────────────────────────────────────────────────┐
 * │                    布隆过滤器内部结构                            │
 * ├─────────────────────────────────────────────────────────────────┤
 * │                                                                 │
 │   [seed_num(4B)] [length(4B)] [seed_0(4B)] [seed_1(4B)] ...      │
 │        │            │           │            │                    │
 │    哈希函数数    位数组长度   种子0        种子1                  │
 │                                                                 │
 │   [对齐填充] [data: uint64_t 位数组...]                          │
 │                                                                 │
 └─────────────────────────────────────────────────────────────────┘
 *
 * @section hash_functions 哈希函数
 *
 * 使用多个哈希函数（由 seed 区分）：
 * - 每个哈希函数是一个多项式哈希：H(s, seed) = Σ(seed^i * s[i])
 * - 通过不同的 seed 生成不同的哈希函数
 * - 元素插入时计算 k 个哈希值，对应位数组的 k 个位置
 * - 查询时检查这 k 个位置是否都被置位
 */

#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "mokv/utils/global_random.h"

namespace mokv {
namespace common {

/**
 * @brief 布隆过滤器类
 *
 * @tparam HashFunction 哈希函数类型（可选，自定义哈希）
 *
 * 使用示例：
 * @code
 * BloomFilter bf;
 * bf.Init(10000, 0.01);  // 预期10000个元素，1%误判率
 * bf.Insert("key1");
 * bf.Insert("key2");
 * bool exists = bf.Check("key1");  // true
 * bool exists = bf.Check("key3");  // 可能是 false（真阴性）或 true（假阳性）
 * @endcode
 */
class BloomFilter {
public:
    /**
     * @brief 获取布隆过滤器的二进制大小（用于序列化）
     * @return 序列化所需的字节数
     *
     * 计算公式：
     * binary_size = 2 * sizeof(size_t) + hash_num * sizeof(size_t)
     *             + 对齐填充 + size_ * sizeof(uint64_t)
     */
    size_t binary_size() {
        size_t index = seed_.size() * sizeof(size_t) + 2 * sizeof(size_t);
        size_t align = sizeof(uint64_t) - (index & (sizeof(uint64_t) - 1));
        return seed_.size() * sizeof(size_t) + 2 * sizeof(size_t)
             + size_ * sizeof(uint64_t) + align;
    }

    /// @brief 获取位数组长度
    size_t length() const { return length_; }

    /**
     * @brief 从内存加载布隆过滤器
     * @param s 指向序列化数据的指针
     * @return 读取的字节数
     *
     * @note 此函数不分配 data_ 内存，而是直接指向 s 的某个位置
     *       因此 s 指向的内存必须在 BloomFilter 生命周期内保持有效
     */
    size_t Load(char* s) {
        size_t index = 0;
        hash_num_ = *reinterpret_cast<size_t*>(s);
        index += sizeof(size_t);
        length_ = *reinterpret_cast<size_t*>(s + index);
        index += sizeof(size_t);

        // 加载哈希种子
        seed_.clear();
        seed_.reserve(hash_num_);
        for (size_t i = 0; i < hash_num_; ++i) {
            seed_.emplace_back(*reinterpret_cast<size_t*>(s + index));
            index += sizeof(size_t);
        }

        // 计算位数组大小
        size_ = length_ / 64 + 1;

        // 对齐填充
        index += sizeof(uint64_t) - (index & (sizeof(uint64_t) - 1));

        // 直接指向数据（不复制）
        data_ = reinterpret_cast<uint64_t*>(s + index);
        index += size_ * sizeof(int64_t);
        loaded_ = true;
        return index;
    }

    /**
     * @brief 将布隆过滤器序列化到内存
     * @param s 目标内存指针
     * @return 写入的字节数
     */
    size_t Save(char* s) {
        size_t index = 0;
        *reinterpret_cast<size_t*>(s) = hash_num_;
        index += sizeof(size_t);
        *reinterpret_cast<size_t*>(s + index) = length_;
        index += sizeof(size_t);

        // 保存哈希种子
        for (size_t i = 0; i < hash_num_; ++i) {
            *reinterpret_cast<size_t*>(s + index) = seed_[i];
            index += sizeof(size_t);
        }

        // 对齐填充
        index += sizeof(uint64_t) - (index & (sizeof(uint64_t) - 1));

        // 复制位数组数据
        memcpy(s + index, reinterpret_cast<char*>(data_), size_ * sizeof(uint64_t));
        return index + size_ * sizeof(uint64_t);
    }

    /**
     * @brief 初始化布隆过滤器
     * @param n 预期插入的元素数量
     * @param p 期望的最大误判率 (0 < p < 1)，默认 0.01 (1%)
     *
     * 计算公式：
     * - length = -n * ln(p) / (ln(2)^2) * 2.35
     * - hash_num = max(1, 0.69 * length / n)
     *
     * 系数 2.35 提供额外的安全裕度
     */
    void Init(size_t n, double p = 0.01) {
        length_ = CalcLength(n, p);
        hash_num_ = std::max(1, int(0.69 * length_ / n));
        seed_.reserve(hash_num_);
        for (size_t i = 0; i < hash_num_; ++i) {
            seed_.emplace_back(cpputil::common::GlobalRand());
        }
        size_ = length_ / 64 + 1;
        data_ = new uint64_t[size_]();  // 零初始化
    }

    /**
     * @brief 插入元素（C字符串版本）
     * @param s 字符串指针
     * @param len 字符串长度
     *
     * @note 多次插入同一个元素是安全的（只会设置更多的位）
     */
    void Insert(const char* s, size_t len) {
        for (auto seed : seed_) {
            size_t key = CalcHash(s, len, seed) % length_;
            data_[key / 64] |= 1ULL << (key & 63);
        }
    }

    /**
     * @brief 插入元素（string_view 版本）
     * @param s 字符串视图
     * @param len 字符串长度
     */
    void Insert(std::string_view s, size_t len) {
        for (auto seed : seed_) {
            size_t key = CalcHash(s, len, seed) % length_;
            data_[key / 64] |= 1ULL << (key & 63);
        }
    }

    /**
     * @brief 检查元素是否存在
     * @param s 字符串指针
     * @param len 字符串长度
     * @return true 表示元素可能存在，false 表示元素一定不存在
     *
     * @section false_positive 假阳性说明
     *
     * 如果元素不存在，Check 一定返回 false。
     * 如果元素存在，Check 很可能返回 true（但有误判可能）。
     * 误判率取决于初始化时指定的 p 值。
     */
    bool Check(const char* s, size_t len) {
        for (auto seed : seed_) {
            size_t key = CalcHash(s, len, seed) % length_;
            if (!(data_[key / 64] & (1ULL << (key & 63)))) {
                return false;  // 一定不存在
            }
        }
        return true;  // 可能存在（可能是假阳性）
    }

    /// @brief 析构函数，释放动态分配的内存
    ~BloomFilter() {
        if (!loaded_ && data_) {
            delete[] data_;
        }
    }

private:
    /**
     * @brief 计算字符串的哈希值（C字符串版本）
     * @param s 字符串指针
     * @param len 字符串长度
     * @param seed 哈希种子
     * @return 哈希值
     *
     * 使用多项式哈希：H(s) = Σ(seed^i * s[i])
     */
    size_t CalcHash(const char* s, size_t len, size_t seed) {
        size_t res = 0;
        for (size_t i = 0; i < len; ++i) {
            res *= seed;
            res += static_cast<size_t>(s[i]);
        }
        return res;
    }

    /**
     * @brief 计算字符串的哈希值（string_view 版本）
     */
    size_t CalcHash(std::string_view s, size_t len, size_t seed) {
        size_t res = 0;
        for (size_t i = 0; i < len; ++i) {
            res *= seed;
            res += static_cast<size_t>(s[i]);
        }
        return res;
    }

    /**
     * @brief 根据元素数量和误判率计算位数组长度
     * @param n 预期元素数量
     * @param p 期望误判率
     * @return 位数组长度
     *
     * 数学推导：
     * m = -n * ln(p) / (ln(2))^2
     *
     * 当 p = 0.01 时，每个元素需要约 9.6 位
     */
    size_t CalcLength(size_t n, double p) {
        return static_cast<size_t>(
            -std::log(p) * static_cast<double>(n) / std::log(2) / std::log(2) * 2.35
        ) + 1;
    }

private:
    size_t length_{0};                  ///< 位数组长度
    size_t hash_num_{0};                ///< 哈希函数数量
    std::vector<size_t> seed_;          ///< 哈希种子列表
    uint64_t* data_{nullptr};           ///< 位数组指针
    size_t size_{0};                    ///< 位数组大小（uint64_t 数量）
    bool loaded_{false};                ///< 是否从外部加载数据
};

}  // namespace common
}  // namespace mokv
