#pragma once
/**
 * @file concurrent_cache.hpp
 * @brief 并发安全的 LRU 缓存实现
 *
 * 基于 boost::concurrent_flat_map 实现的高性能线程安全缓存。
 * 采用延迟提升（Lazy Promotion）策略减少锁竞争。
 *
 * @section architecture 架构设计
 * ┌─────────────────────────────────────────────────────────────┐
 * │                   ConcurrentLRUCache                         │
 * ├─────────────────────────────────────────────────────────────┤
 * │                                                                 │
 * │   ┌─────────────┐    ┌─────────────────────────────────┐    │
 * │   │ HashTable   │    │        LRU 链表                  │    │
 * │   │ (并发安全)   │◄──►│  [MRU] ─→ ... ─→ [LRU]          │    │
 * │   │ O(1) 查找    │    │                                 │    │
 * │   └─────────────┘    └─────────────────────────────────┘    │
 * │                                                                 │
 * @section lazy_promotion 延迟提升策略
 *
 * 为避免热点数据频繁移动到链表头部，采用延迟提升：
 * - 访问时只增加 promotions 计数器
 * - 只有达到阈值（should_promote_num_）才真正提升
 * - 减少链表操作开销，提高并发性能
 */

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <boost/unordered/concurrent_flat_map.hpp>
#include "mokv/cache/list.hpp"
#include "mokv/cache/cm_sketch.hpp"

namespace cpputil {

namespace cache {

/**
 * @class Cache
 * @brief 缓存基类
 */
class Cache {
public:
    template <typename T>
    Cache(T&& name) {
        name_ = std::forward<T>(name);
    }
    const std::string& name() {
        return name_;
    }
protected:
    std::string name_;
};

/**
 * @brief 类型传递优化
 *
 * 对于大于 16 字节或非平凡可拷贝的类型，使用引用传递
 * 以减少拷贝开销
 */
template <typename T>
struct PassBy {
    using type = typename std::conditional<(sizeof(T) > (sizeof(void*) << 1)) || !std::is_trivially_copyable_v<T>, T&, T>::type;
};

/**
 * @class ConcurrentLRUCache
 * @brief 线程安全的 LRU 缓存
 *
 * 使用 boost::concurrent_flat_map 提供并发安全的 O(1) 查找，
 * 配合双向链表维护 LRU 顺序。
 *
 * @tparam TKey 键类型
 * @tparam TValue 值类型
 * @tparam TMap 底层哈希表类型（默认使用 boost::concurrent_flat_map）
 */
template <typename TKey, typename TValue = TKey, typename TMap = boost::concurrent_flat_map<TKey, TValue> >
class ConcurrentLRUCache {
    using TNode = cpputil::list::Node<std::shared_ptr<TValue> >;

    
    // struct HashCompare {
    //     static size_t Hash(const typename PassBy<TKey>::type key) {
    //         return THash()(key);
    //     }
    //     static bool Equal(const typename PassBy<TKey>::type lhs, const typename PassBy<TKey>::type rhs) {
    //         return TKeyEqual()(lhs, rhs);
    //     }
    // };
private:
    inline bool ShouldPromote(TNode* node) { // atomic
        // This counter is only a hotness heuristic; it does not synchronize any
        // other shared state, so relaxed ordering is sufficient here.
        return node->promotions.fetch_add(1, std::memory_order_relaxed) >= should_promote_num_;
    }

    inline void ResetPromote(TNode* node) {
        node->promotions.store(0, std::memory_order_relaxed);
    }

    inline void PromoteNoLock(TNode* node) {
        list_.Extract(node);
        list_.InsertFront(node);
        ResetPromote(node);
    }

    inline void Promote(TNode* node) noexcept {
        // std::unique_lock<std::mutex> lock(list_mutex_);
        if (ShouldPromote(node)) { // atomic
            // std::unique_lock<std::mutex> lock(list_mutex_);
            PromoteNoLock(node);
        }
    }
public:

    ConcurrentLRUCache(size_t capacity = 24, size_t should_promote_num = 8): capacity_(capacity), should_promote_num_(should_promote_num) {

    }

    void Reserve(size_t capacity) {
        this->capacity_ = capacity;
        map_.reserve(capacity);
    }

    void Put(const typename PassBy<TValue>::type value) {
        bool exist = false;
        {
            std::unique_lock<std::mutex> lock(list_mutex_);
            map_.visit(static_cast<TKey>(value), [&](auto& x) {
                Promote(x.second);
                exist = true;
            });
        }

        if (exist) {
            return;
        }

        TNode* node_ptr;
        {
            std::unique_lock<std::mutex> lock(list_mutex_);
            // std::unique_lock<std::mutex> lock(list_mutex_); // setting the same value concurrently produces garbage data 
            if (list_.size() == capacity_) {
                auto node_ptr = list_.PopBack();
                map_.erase(static_cast<TKey>(*(node_ptr->value)));
            }
            auto value_ptr = std::make_shared<TValue>(value);
            node_ptr = list_.PushFront(std::move(value_ptr));
            map_.emplace(static_cast<TKey>(value), node_ptr);
        }
    }

    std::shared_ptr<TValue> PutWithDisuse(std::shared_ptr<TValue> value) {
        bool exist = false;
        {
            std::unique_lock<std::mutex> lock(list_mutex_);
            map_.visit(static_cast<TKey>(*value), [&](auto& x) {
                Promote(x.second);
                exist = true;
            });
        }

        if (exist) {
            return nullptr;
        }

        std::shared_ptr<TValue> res_value_ptr;
        {
            std::unique_lock<std::mutex> lock(list_mutex_);
            if (list_.size() == capacity_) {
                res_value_ptr = list_.PopBack()->value;
                map_.erase(static_cast<TKey>(*res_value_ptr));
            }
            auto node_ptr = list_.PushFront(value);
            map_.emplace(static_cast<TKey>(*value), node_ptr);
        }
        return res_value_ptr;
    }

    // if compair is true, put new value
    void PutWithCompair(std::shared_ptr<TValue> value, std::function<bool(const typename PassBy<TValue>::type, const typename PassBy<TValue>::type)> compair) {
        bool exist = false;
        {
            std::unique_lock<std::mutex> lock(list_mutex_);
            map_.visit(static_cast<TKey>(*value), [&](auto& x) {
                Promote(x.second);
                exist = true;
            });
        }

        if (exist) {
            return;
        }

        std::shared_ptr<TValue> res_value_ptr;
        {
            std::unique_lock<std::mutex> lock(list_mutex_);
            if (list_.size() == capacity_) {
                auto it = list_.end();
                --it;
                if (compair(**it, *value)) {
                    res_value_ptr = list_.PopBack()->value;
                    map_.erase(static_cast<TKey>(*res_value_ptr));
                } else {
                    return;
                }
            }
            auto node_ptr = list_.PushFront(value);
            map_.emplace(static_cast<TKey>(*value), node_ptr);
        }
        return;
    }

    
    template <typename U = TValue, typename std::enable_if<!std::is_same_v<U, typename PassBy<TValue>::type>, int>::type = 0>
    void Put(TValue&& value) {
        bool exist = false;
        {
            std::unique_lock<std::mutex> lock(list_mutex_);
            map_.visit(static_cast<TKey>(value), [&](auto& x) {
                // Promote 使用原子操作优化，避免频繁加锁
                // 只有访问次数达到阈值才真正移动节点
                Promote(x.second);
                exist = true;
            });
        }

        if (exist) {
            return;
        }

        TNode* node_ptr;
        {
            std::unique_lock<std::mutex> lock(list_mutex_); // setting the same value concurrently produces garbage data 
            if (list_.size() == capacity_) {
                auto node_ptr = list_.PopBack();
                map_.erase(static_cast<TKey>(*(node_ptr->value)));
            }
            auto value_ptr = std::make_shared<TValue>(std::move(value));
            node_ptr = list_.PushFront(std::move(value_ptr));
        }
        map_.emplace(static_cast<TKey>(*(node_ptr->value)), node_ptr);
    }

    std::shared_ptr<TValue> Get(const typename PassBy<TKey>::type key) {
        std::shared_ptr<TValue> res;
        {
            std::unique_lock<std::mutex> lock(list_mutex_);
            map_.visit(key, [&](const auto& x) {
                res = x.second->value;
                Promote(x.second);
            });
        }
        return res;
    }

    std::shared_ptr<TValue> Peek(const typename PassBy<TKey>::type key) {
        std::shared_ptr<TValue> res;
        map_.visit(key, [&](const auto& x) {
            res = x.second->value;
        });
        return res;
    }

    size_t TrueSize() const {
        return list_.size();
    }

    size_t size() const {
        return map_.size();
    }
private:
    size_t capacity_;
    size_t should_promote_num_;
    boost::concurrent_flat_map<TKey, TNode*> map_;
    cpputil::list::List<std::shared_ptr<TValue> > list_;
    std::mutex list_mutex_;
};

// need Value to Key conversion constructor
template <typename TKey, typename TValue = TKey, typename TMap = boost::concurrent_flat_map<TKey, TValue>,
    size_t ShardBits = 6, typename THash = std::hash<TKey> >
class ConcurrentBucketLRUCache : public Cache {
    constexpr const static size_t shard_num_ = 1 << ShardBits;
    constexpr const static size_t shard_mask_ = shard_num_ - 1;
    using TShard = ConcurrentLRUCache<TKey, TValue, TMap>;

private:
    TShard& GetShard(const typename PassBy<TKey>::type key) {
        auto index = THash()(key) & shard_mask_;
        return shard_[index];
    }
    
public:
    ConcurrentBucketLRUCache(const std::string& name, size_t capacity = 1024): Cache(name) {
        capacity_ = capacity;
        for (size_t i = 0; i < shard_num_; i++) {
            shard_[i].Reserve((capacity >> ShardBits) + 1);
        }
    }
    
    void Put(const typename PassBy<TValue>::type value) {
        auto& shard= GetShard(TKey(value));
        shard.Put(value);
    }


    template <typename U = TValue, typename std::enable_if<!std::is_same_v<U, typename PassBy<TValue>::type>, int>::type = 0>
    void Put(TValue&& value) {
        auto& shard = GetShard(TKey(value));
        shard.Put(std::move(value));
    }

    std::shared_ptr<TValue> Get(const typename PassBy<TKey>::type key) {
        auto& shard = GetShard(key);
        return shard.Get(key);
    }

    std::shared_ptr<TValue> Peek(const typename PassBy<TKey>::type key) {
        auto& shard = GetShard(key);
        return shard.Peek(key);
    }

private:
    size_t capacity_;
    std::array<TShard, shard_num_> shard_;
};

template <typename TKey, typename TValue = TKey>
class Concurrent2LRUCache {
public:
    Concurrent2LRUCache& operator = (Concurrent2LRUCache&& rhs) {
        stop();
        delete window_lru_;
        delete main_lru_;
        delete cm_sketch_;
        window_lru_ = rhs.window_lru_;
        rhs.window_lru_ = nullptr;
        main_lru_ = rhs.main_lru_;
        rhs.main_lru_ = nullptr;
        cm_sketch_ = rhs.cm_sketch_;
        rhs.cm_sketch_ = nullptr;
        rhs.stop();
        start();
        return *this;
    }
    Concurrent2LRUCache(size_t capacity = 1024, size_t ratio = 1): window_ratio_(ratio) {
        window_capacity_ = capacity * window_ratio_ / 100;
        if (window_capacity_ == 0) {
            window_capacity_ = 1;
        }
        main_capacity_ = capacity - window_capacity_;
        if (main_capacity_ == 0) {
            main_capacity_ = 1;
        }
        window_lru_ = new ConcurrentLRUCache<TKey, TValue, boost::concurrent_flat_map<TKey, TValue>>(window_capacity_);
        main_lru_ = new ConcurrentLRUCache<TKey, TValue, boost::concurrent_flat_map<TKey, TValue>>(main_capacity_);
        size_t bits_num = 0;
        while (capacity) {
            ++bits_num;
            capacity >>= 1;
        }
        cm_sketch_ = new utils::CMSketch4Bits<TKey>(bits_num);
        start();
    }
    ~Concurrent2LRUCache() {
        stop();
        delete window_lru_;
        delete main_lru_;
        delete cm_sketch_;
    }
    void Put(const typename PassBy<TValue>::type value) {
        {
            std::unique_lock<std::mutex> lock(cm_sketch_mutex_);
            cm_sketch_->Increment(static_cast<TKey>(value));
        }
        auto value_ptr = window_lru_->PutWithDisuse(std::make_shared<TValue>(value));
        if (value_ptr) {
            auto compair = [this](const typename PassBy<TValue>::type old_value, const typename PassBy<TValue>::type new_value) {
                std::unique_lock<std::mutex> lock(cm_sketch_mutex_);
                return cm_sketch_->Estimate(static_cast<TKey>(old_value)) < cm_sketch_->Estimate(static_cast<TKey>(new_value));
            };
            main_lru_->PutWithCompair(value_ptr, compair);
        }
    }

    std::shared_ptr<TValue> Get(const typename PassBy<TKey>::type key) {
        {
            std::unique_lock<std::mutex> lock(cm_sketch_mutex_);
            cm_sketch_->Increment(key);
        }
        auto value_ptr = window_lru_->Get(key);
        if (!value_ptr) {
            value_ptr = main_lru_->Get(key);
        }
        return value_ptr;
    }

    std::shared_ptr<TValue> Peek(const typename PassBy<TKey>::type key) {
        auto value_ptr = window_lru_->Peek(key);
        if (!value_ptr) {
            value_ptr = main_lru_->Peek(key);
        }
        return value_ptr;
    }

private:
    void refresh_loop() {
        while(true) {
            std::unique_lock<std::mutex> lock(refresh_mutex_);
            auto future_time = std::chrono::system_clock::now() + std::chrono::milliseconds(refresh_thread_sleep_ms_);
            refresh_condition_.wait_until(lock, future_time, [this] {
                return stop_;
            });
            if (stop_) {
                return;
            }
            {
                std::unique_lock<std::mutex> lock(cm_sketch_mutex_);
                cm_sketch_->Reset();
            }
        }
    }
    void start() {
        refresh_thread_ = std::thread(&Concurrent2LRUCache::refresh_loop, this);
    }
    void stop() {
        stop_ = true;
        {
            std::unique_lock<std::mutex> lock(refresh_mutex_);
            refresh_condition_.notify_all();
        }
        if (refresh_thread_.joinable()) {
            refresh_thread_.join();
        }
    }
private:
    ConcurrentLRUCache<TKey, TValue>* window_lru_;
    ConcurrentLRUCache<TKey, TValue>* main_lru_;
    size_t window_ratio_ = 1; // 1 ~ 100
    size_t window_size_ = 0;
    size_t window_capacity_ = 0;
    size_t main_capacity_ = 0;
    size_t main_size_ = 0;
    utils::CMSketch4Bits<TKey>* cm_sketch_;
    std::mutex cm_sketch_mutex_;
    std::thread refresh_thread_;
    std::mutex refresh_mutex_;
    std::condition_variable refresh_condition_;
    bool stop_{false};
    int64_t refresh_thread_sleep_ms_ = 60 * 1000ll;
};


// need Value to Key conversion constructor
template <typename TKey, typename TValue = TKey,
    size_t ShardBits = 6, typename THash = std::hash<TKey> >
class ConcurrentBucket2LRUCache : public Cache {
    constexpr const static size_t shard_num_ = 1 << ShardBits;
    constexpr const static size_t shard_mask_ = shard_num_ - 1;
    using TShard = Concurrent2LRUCache<TKey, TValue>;

private:
    TShard& GetShard(const typename PassBy<TKey>::type key) {
        auto index = THash()(key) & shard_mask_;
        return shard_[index];
    }
    
public:
    ConcurrentBucket2LRUCache(const std::string& name, size_t capacity = 1024): Cache(name) {
        capacity_ = capacity;
        for (size_t i = 0; i < shard_num_; i++) {
            shard_[i] = TShard((capacity >> ShardBits) + 1);
        }
    }
    
    void Put(const typename PassBy<TValue>::type value) {
        auto& shard= GetShard(TKey(value));
        shard.Put(value);
    }


    template <typename U = TValue, typename std::enable_if<!std::is_same_v<U, typename PassBy<TValue>::type>, int>::type = 0>
    void Put(TValue&& value) {
        auto& shard = GetShard(TKey(value));
        shard.Put(std::move(value));
    }

    std::shared_ptr<TValue> Get(const typename PassBy<TKey>::type key) {
        auto& shard = GetShard(key);
        return shard.Get(key);
    }

    std::shared_ptr<TValue> Peek(const typename PassBy<TKey>::type key) {
        auto& shard = GetShard(key);
        return shard.Peek(key);
    }

private:
    size_t capacity_;
    std::array<TShard, shard_num_> shard_;
};

} // cache

} // cpputil
