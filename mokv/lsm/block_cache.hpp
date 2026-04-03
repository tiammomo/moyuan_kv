/**
 * @file block_cache.hpp
 * @brief 块缓存实现 - LRU 缓存策略
 *
 * Block Cache 用于缓存 SST 文件的 DataBlock，减少磁盘 I/O：
 * - LRU 淘汰策略：最近最少使用的块优先淘汰
 * - 线程安全：支持多线程并发访问
 * - 命中率统计：监控缓存效果
 *
 * @section cache_structure 缓存结构
 * ┌─────────────────────────────────────────────────────────────────┐
 * │                         BlockCache                              │
 * ├─────────────────────────────────────────────────────────────────┤
 * │                                                                 │
 * │   ┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐    │
 * │   │ Block 1 │    │ Block 2 │    │ Block 3 │    │ Block N │    │
 * │   │ (LRU)   │    │ (LRU)   │    │ (MRU)   │    │ (MRU)   │    │
 * │   └────┬────┘    └────┬────┘    └────┬────┘    └────┬────┘    │
 │   │      │             │             │             │          │
 │   │      └─────────────┴──────┬──────┴─────────────┘          │
 │   │                          │                                 │
 │   │                    ┌──────▼──────┐                         │
 │   │                    │   HashMap   │                         │
 │   │                    │  (O(1) 查找) │                         │
 │   │                    └─────────────┘                         │
 │   │                                                                 │
 │   └─────────────────────────────────────────────────────────────────┘
 *
 * @section eviction_policy 淘汰策略
 *
 * 使用 LRU-K 或简单 LRU：
 * - 访问时将块移动到链表头部
 * - 淘汰时从链表尾部移除
 * - 结合哈希表实现 O(1) 时间复杂度的查找和淘汰
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace mokv {
namespace lsm {

/**
 * @brief 缓存块结构
 */
struct CacheBlock {
    CacheBlock(std::vector<char>&& data, size_t sst_id, size_t block_offset)
        : data_(std::move(data)), sst_id_(sst_id), block_offset_(block_offset),
          access_time_(0), hit_count_(0) {}

    std::vector<char> data_;       ///< 块数据
    size_t sst_id_;                ///< SST 文件 ID
    size_t block_offset_;          ///< 块在 SST 中的偏移量
    uint64_t access_time_;         ///< 访问时间戳
    size_t hit_count_{0};          ///< 命中次数

    /// @brief 获取数据指针
    const char* data() const { return data_.data(); }

    /// @brief 获取数据大小
    size_t size() const { return data_.size(); }
};

/**
 * @brief Block Cache 类
 *
 * 提供线程安全的块缓存，支持：
 * - LRU 淘汰策略
 * - 命中率统计
 * - 容量限制
 *
 * @tparam KeyType 缓存键类型
 */
class BlockCache {
public:
    using Key = uint64_t;  ///< 缓存键：(sst_id << 32) | block_offset

    /**
     * @brief 缓存配置
     */
    struct Config {
        /// @brief 最大缓存容量（字节）
        size_t max_capacity;

        /// @brief 最小块大小
        size_t min_block_size;

        /// @brief 最大块大小
        size_t max_block_size;

        /// @brief 缓存块数量限制（0 表示不限制）
        size_t max_block_count;

        /// @brief 拒绝缓存的最小利用率（数据大小/缓存大小）
        double min_utilization;

        Config()
            : max_capacity(256 * 1024 * 1024)  // 256MB
            , min_block_size(4096)
            , max_block_size(64 * 1024)        // 64KB
            , max_block_count(0)
            , min_utilization(0.5) {}
    };

    BlockCache() : config_() {}
    explicit BlockCache(const Config& config) : config_(config) {}

    /// @brief 禁止拷贝
    BlockCache(const BlockCache&) = delete;
    BlockCache& operator=(const BlockCache&) = delete;

    /**
     * @brief 获取缓存块
     * @param sst_id SST 文件 ID
     * @param block_offset 块偏移量
     * @return 缓存块数据指针，未命中返回 nullptr
     */
    const std::vector<char>* Get(size_t sst_id, size_t block_offset) {
        Key key = MakeKey(sst_id, block_offset);

        std::unique_lock<std::mutex> lock(mutex_);

        auto it = cache_map_.find(key);
        if (it != cache_map_.end()) {
            // 命中
            CacheBlock& block = *it->second;
            block.hit_count_++;
            block.access_time_ = ++access_counter_;

            // 移动到链表头部（MRU）
            MoveToFront(it->second);

            stats_.hit_count++;
            stats_.total_access++;
            return &block.data_;
        }

        // 未命中
        stats_.miss_count++;
        stats_.total_access++;
        return nullptr;
    }

    /**
     * @brief 插入缓存块
     * @param sst_id SST 文件 ID
     * @param block_offset 块偏移量
     * @param data 块数据
     * @return true 插入成功，false 拒绝缓存
     */
    bool Put(size_t sst_id, size_t block_offset, std::vector<char>&& data) {
        if (data.empty()) {
            return false;
        }

        // 检查数据大小
        if (data.size() > config_.max_block_size) {
            stats_.rejected_count++;
            return false;  // 数据块过大
        }

        // 计算缓存利用率
        double utilization = static_cast<double>(data.size()) / config_.min_block_size;
        if (utilization < config_.min_utilization) {
            stats_.rejected_count++;
            return false;  // 利用率过低
        }

        std::unique_lock<std::mutex> lock(mutex_);

        Key key = MakeKey(sst_id, block_offset);

        // 检查是否已存在
        auto it = cache_map_.find(key);
        if (it != cache_map_.end()) {
            // 更新已有块
            CacheBlock& block = *it->second;
            block.data_ = std::move(data);
            block.access_time_ = ++access_counter_;
            MoveToFront(it->second);
            return true;
        }

        // 检查容量
        EvictIfNeeded(data.size());

        // 插入新块
        auto block_ptr = std::make_shared<CacheBlock>(std::move(data), sst_id, block_offset);
        block_ptr->access_time_ = ++access_counter_;

        cache_map_[key] = block_ptr;
        lru_list_.push_front(block_ptr);

        stats_.current_size += block_ptr->size();
        stats_.current_count++;

        return true;
    }

    /**
     * @brief 移除缓存块
     * @param sst_id SST 文件 ID
     * @param block_offset 块偏移量
     * @return true 移除成功
     */
    bool Remove(size_t sst_id, size_t block_offset) {
        Key key = MakeKey(sst_id, block_offset);

        std::unique_lock<std::mutex> lock(mutex_);

        auto it = cache_map_.find(key);
        if (it != cache_map_.end()) {
            RemoveBlock(it->second);
            cache_map_.erase(it);
            return true;
        }
        return false;
    }

    /// @brief 清空缓存
    void Clear() {
        std::unique_lock<std::mutex> lock(mutex_);
        cache_map_.clear();
        lru_list_.clear();
        stats_.current_size = 0;
        stats_.current_count = 0;
    }

    /**
     * @brief 获取缓存统计信息
     */
    struct Stats {
        size_t total_access{0};     ///< 总访问次数
        size_t hit_count{0};        ///< 命中次数
        size_t miss_count{0};       ///< 未命中次数
        size_t rejected_count{0};   ///< 被拒绝的插入次数
        size_t evicted_count{0};    ///< 淘汰的块数
        size_t current_size{0};     ///< 当前缓存大小（字节）
        size_t current_count{0};    ///< 当前缓存块数
        uint64_t access_counter{0}; ///< 访问计数器

        /// @brief 计算命中率
        double HitRate() const {
            return total_access > 0 ?
                   static_cast<double>(hit_count) / total_access : 0.0;
        }
    };

    /// @brief 获取统计信息
    const Stats& GetStats() const { return stats_; }

    /// @brief 重置统计信息
    void ResetStats() {
        stats_ = Stats();
    }

    /// @brief 获取当前缓存大小（字节）
    size_t CurrentSize() const { return stats_.current_size; }

    /// @brief 获取当前缓存块数
    size_t CurrentCount() const { return stats_.current_count; }

    /// @brief 获取配置
    const Config& GetConfig() const { return config_; }

private:
    static Key MakeKey(size_t sst_id, size_t block_offset) {
        return (static_cast<Key>(sst_id) << 32) | static_cast<Key>(block_offset);
    }

    void MoveToFront(std::shared_ptr<CacheBlock> block) {
        lru_list_.remove(block);
        lru_list_.push_front(block);
    }

    void RemoveBlock(std::shared_ptr<CacheBlock> block) {
        lru_list_.remove(block);
        stats_.current_size -= block->size();
        stats_.current_count--;
    }

    void EvictIfNeeded(size_t new_block_size) {
        // 检查容量限制
        while (stats_.current_size + new_block_size > config_.max_capacity ||
               (config_.max_block_count > 0 && stats_.current_count >= config_.max_block_count)) {

            if (lru_list_.empty()) {
                break;
            }

            // 移除最少使用的块（LRU）
            auto lru_block = lru_list_.back();
            Key key = MakeKey(lru_block->sst_id_, lru_block->block_offset_);

            RemoveBlock(lru_block);
            cache_map_.erase(key);
            stats_.evicted_count++;
        }
    }

private:
    Config config_;
    std::unordered_map<Key, std::shared_ptr<CacheBlock>> cache_map_;  ///< 哈希表
    std::list<std::shared_ptr<CacheBlock>> lru_list_;                  ///< LRU 链表
    std::mutex mutex_;
    Stats stats_;
    uint64_t access_counter_{0};
};

/**
 * @brief 全局 Block Cache 实例
 *
 * 提供单例模式的全局缓存，供整个数据库使用。
 */
class GlobalBlockCache {
public:
    /// @brief 获取单例实例
    static BlockCache& Instance() {
        static BlockCache instance;
        return instance;
    }

    // 禁止拷贝
    GlobalBlockCache(const GlobalBlockCache&) = delete;
    GlobalBlockCache& operator=(const GlobalBlockCache&) = delete;

private:
    GlobalBlockCache() = default;
};

/**
 * @brief 缓存管理器辅助类
 *
 * RAII 风格的缓存操作，确保缓存操作正确完成。
 */
class CacheGuard {
public:
    CacheGuard(BlockCache& cache, size_t sst_id, size_t block_offset)
        : cache_(cache), sst_id_(sst_id), block_offset_(block_offset), hit_(false) {
        data_ = cache_.Get(sst_id, block_offset);
        if (data_) {
            hit_ = true;
        }
    }

    ~CacheGuard() {
        if (!hit_ && !pending_data_.empty()) {
            // 如果之前未命中但有数据，插入缓存
            cache_.Put(sst_id_, block_offset_, std::move(pending_data_));
        }
    }

    /// @brief 设置要缓存的数据
    void SetData(std::vector<char>&& data) {
        if (!hit_) {
            pending_data_ = std::move(data);
        }
    }

    /// @brief 检查是否命中
    bool Hit() const { return hit_; }

    /// @brief 获取数据指针
    const std::vector<char>* Data() const { return data_; }

    /// @brief 获取数据（如果未命中，返回 pending_data）
    const std::vector<char>* DataOrPending() const {
        return data_ ? data_ : &pending_data_;
    }

private:
    BlockCache& cache_;
    size_t sst_id_;
    size_t block_offset_;
    bool hit_;
    const std::vector<char>* data_{nullptr};
    std::vector<char> pending_data_;
};

}  // namespace lsm
}  // namespace mokv
