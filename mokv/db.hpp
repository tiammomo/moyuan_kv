#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mokv/lsm/manifest.hpp"
#include "mokv/lsm/memtable.hpp"
#include "mokv/lsm/sst.hpp"
#include "mokv/lsm/block_cache.hpp"
#include "mokv/pool/thread_pool.hpp"
#include "mokv/utils/lock.hpp"

namespace mokv {

namespace detail {

inline std::string_view TombstoneValue() {
    static constexpr char kTombstone[] = "\x1E__mokv_deleted__\x1F";
    return std::string_view(kTombstone, sizeof(kTombstone) - 1);
}

inline bool IsTombstone(std::string_view value) {
    return value == TombstoneValue();
}

}  // namespace detail

/**
 * @brief DB 配置
 */
struct DBConfig {
    /// @brief 压缩配置
    lsm::CompressionConfig compression;

    /// @brief Block Cache 配置
    lsm::BlockCache::Config block_cache;

    /// @brief MemTable 最大大小: 3MB (小于页面大小以优化IO)
    size_t memtable_max_size = 4096 * 1024 - 1024 * 1024;

    /// @brief 是否启用 Block Cache
    bool enable_block_cache = true;
};

class DB {
public:
    DB(const DBConfig& config = DBConfig()) : config_(config) {
        memtable_ = std::make_shared<lsm::MemTable>();
        to_sst_thread_ = std::thread(&DB::ToSSTLoop, this);
        manifest_queue_.emplace_back(std::make_shared<lsm::Manifest>());
        sst_id_ = manifest_queue_.back()->max_sst_id();

        // 初始化 Block Cache
        if (config_.enable_block_cache) {
            block_cache_ = std::make_unique<lsm::BlockCache>(config_.block_cache);
        }
    }

    ~DB() {
        std::shared_ptr<lsm::MemTable> pending_memtable;
        {
            common::RWLock::WriteLock w_lock(memtable_lock_);
            if (memtable_->size() > 0) {
                pending_memtable = memtable_;
                memtable_ = std::make_shared<lsm::MemTable>();
            }
        }
        {
            std::unique_lock<std::mutex> lock(flush_mutex_);
            if (pending_memtable) {
                flush_queue_.emplace_back(std::move(pending_memtable));
            }
            to_sst_stop_flag_ = true;
            to_sst_cv_.notify_all();
        }
        if (to_sst_thread_.joinable()) {
            to_sst_thread_.join();
        }
        {
            common::RWLock::WriteLock w_lock(manifest_lock_);
            manifest_queue_.back()->Save();
        }
    }

    bool Get(std::string_view key, std::string& value) {
        {
            common::RWLock::ReadLock r_lock(memtable_lock_);
            if (memtable_->Get(key, value)) {
                if (detail::IsTombstone(value)) {
                    value.clear();
                    return false;
                }
                return true;
            }
        }
        {
            std::lock_guard<std::mutex> lock(flush_mutex_);
            for (auto it = flush_queue_.rbegin(); it != flush_queue_.rend(); ++it) {
                if ((*it)->Get(key, value)) {
                    if (detail::IsTombstone(value)) {
                        value.clear();
                        return false;
                    }
                    return true;
                }
            }
        }
        {
            common::RWLock::ReadLock r_lock(manifest_lock_);
            const bool found = manifest_queue_.back()->Get(key, value);
            if (found && detail::IsTombstone(value)) {
                value.clear();
                return false;
            }
            return found;
        }
    }

    void Put(std::string_view key, std::string_view value) {
        std::shared_ptr<lsm::MemTable> pending_flush;
        {
            common::RWLock::ReadLock r_lock(memtable_lock_);
            memtable_->Put(key, value);
            if (memtable_->binary_size() <= config_.memtable_max_size) {
                return;
            }
        }
        {
            common::RWLock::WriteLock w_lock(memtable_lock_);
            if (memtable_->binary_size() > config_.memtable_max_size) {
                pending_flush = memtable_;
                memtable_ = std::make_shared<lsm::MemTable>();
            }
        }
        if (pending_flush) {
            std::lock_guard<std::mutex> lock(flush_mutex_);
            flush_queue_.emplace_back(std::move(pending_flush));
            to_sst_cv_.notify_one();
        }
    }

    void Delete(std::string_view key) {
        Put(key, detail::TombstoneValue());
    }

    bool Exists(std::string_view key) {
        std::string value;
        return Get(key, value);
    }

    /// @brief 获取压缩率统计
    double GetCompressionRatio() const {
        common::RWLock::ReadLock r_lock(manifest_lock_);
        size_t total_size = 0, uncompressed_size = 0;
        for (auto& manifest : manifest_queue_) {
            for (size_t i = 0; i < manifest->levels().size(); ++i) {
                for (auto& sst : manifest->levels()[i].ssts()) {
                    total_size += sst->binary_size();
                    uncompressed_size += sst->binary_size() * 2;  // 假设压缩比 2x
                }
            }
        }
        return total_size > 0 ? static_cast<double>(total_size) / uncompressed_size : 1.0;
    }
private:
    void ToSSTLoop() {
        while (true) {
            std::unique_lock<std::mutex> lock(flush_mutex_);
            // 等待直到有数据需要刷盘或收到停止信号
            to_sst_cv_.wait(lock, [this] {
                return to_sst_stop_flag_ || !flush_queue_.empty();
            });
            if (to_sst_stop_flag_ && flush_queue_.empty()) {
                break;
            }
            // 处理所有待刷盘的 memtable
            while (!flush_queue_.empty()) {
                auto memtable = flush_queue_.front();
                flush_queue_.erase(flush_queue_.begin());
                lock.unlock();  // 释放锁以避免阻塞 Put 操作
                ToSST(memtable);
                lock.lock();    // 重新获取锁
            }
        }
    }

    // 将 memtable 刷盘为 SST 文件（支持压缩和缓存）
    void ToSST(std::shared_ptr<lsm::MemeTable> inmemtable) {
        const size_t next_sst_id = ++sst_id_;
        std::shared_ptr<lsm::SST> sst;
        if (config_.compression.enable) {
            sst = std::make_shared<lsm::SST>(*inmemtable, next_sst_id, config_.compression);
        } else {
            sst = std::make_shared<lsm::SST>(*inmemtable, next_sst_id);
        }

        // 设置 Block Cache
        if (block_cache_) {
            sst->SetBlockCache(block_cache_.get());
        }

        {
            common::RWLock::WriteLock w_lock(manifest_lock_);
            auto new_manifest = manifest_queue_.back()->InsertAndUpdate(sst);
            if (new_manifest->CanDoCompaction()) {
                new_manifest->SizeTieredCompaction(++sst_id_, config_.compression);
            }
            manifest_queue_.emplace_back(new_manifest);
        }
    }

    // ============ 缓存相关接口 ============

    /// @brief 获取 Block Cache 统计信息
    const lsm::BlockCache::Stats& GetCacheStats() const {
        static lsm::BlockCache::Stats empty_stats;
        return block_cache_ ? block_cache_->GetStats() : empty_stats;
    }

    /// @brief 获取缓存命中率
    double GetCacheHitRate() const {
        return block_cache_ ? block_cache_->GetStats().HitRate() : 0.0;
    }

    /// @brief 清空 Block Cache
    void ClearCache() {
        if (block_cache_) {
            block_cache_->Clear();
        }
    }

    /// @brief 获取当前缓存大小
    size_t GetCacheSize() const {
        return block_cache_ ? block_cache_->CurrentSize() : 0;
    }

private:
    DBConfig config_;  ///< 数据库配置
    std::shared_ptr<lsm::MemTable> memtable_;
    std::vector<std::shared_ptr<mokv::lsm::Manifest> > manifest_queue_;
    mutable common::RWLock manifest_lock_;
    common::RWLock memtable_lock_;

    std::unique_ptr<lsm::BlockCache> block_cache_;  ///< Block Cache

    std::thread to_sst_thread_;
    std::mutex flush_mutex_;
    std::condition_variable to_sst_cv_;
    std::vector<std::shared_ptr<lsm::MemTable> > flush_queue_;
    bool to_sst_stop_flag_ = false;

    std::atomic_size_t sst_id_{0};
};

}  // namespace mokv
