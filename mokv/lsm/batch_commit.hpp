/**
 * @file batch_commit.hpp
 * @brief 批量提交实现 - 事务性批量写入
 *
 * 支持将多个写操作批量提交：
 * - 批量 Put 操作
 * - 原子性提交（全部成功或全部失败）
 * - 批量删除
 *
 * @section batch_flow 批量提交流程
 * ┌─────────────────────────────────────────────────────────────────┐
 * │                    批量提交流程                                  │
 * ├─────────────────────────────────────────────────────────────────┤
 * │                                                                 │
 │  1. BeginBatch() - 开始批量操作                                   │
 *  │        ↓                                                       │
 │  2. BatchPut(key, value) - 添加 Put 操作                          │
 │  │        ↓                                                       │
 │  3. BatchDelete(key) - 添加 Delete 操作                           │
 *  │        ↓                                                       │
 │  4. CommitBatch() - 原子性提交                                    │
 *  │        ↓                                                       │
 │  5. 应用到 MemTable 和 WAL                                        │
 *                                                                 │
 └─────────────────────────────────────────────────────────────────┘
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mokv/lsm/memtable.hpp"

namespace mokv {
namespace lsm {

/**
 * @brief 批量操作条目
 */
struct BatchEntry {
    enum Type { kPut, kDelete } type;
    std::string key;
    std::string value;  // 仅 Put 使用

    static BatchEntry MakePut(std::string_view key, std::string_view value) {
        BatchEntry entry;
        entry.type = kPut;
        entry.key = std::string(key);
        entry.value = std::string(value);
        return entry;
    }

    static BatchEntry MakeDelete(std::string_view key) {
        BatchEntry entry;
        entry.type = kDelete;
        entry.key = std::string(key);
        return entry;
    }
};

/**
 * @brief 批量提交状态
 */
enum class BatchStatus {
    kIdle,        ///< 空闲
    kActive,      ///< 活跃（正在收集操作）
    kCommitting,  ///< 正在提交
    kRolledBack   ///< 已回滚
};

/**
 * @brief 批量提交上下文
 *
 * 用于收集批量操作，并在提交时原子性应用。
 */
class BatchCommit {
public:
    BatchCommit() : status_(BatchStatus::kIdle), size_(0) {}

    /// @brief 开始批量操作
    void Begin() {
        status_ = BatchStatus::kActive;
        entries_.clear();
        size_ = 0;
    }

    /// @brief 添加 Put 操作
    void BatchPut(std::string_view key, std::string_view value) {
        if (status_ != BatchStatus::kActive) return;

        entries_.emplace_back(BatchEntry::MakePut(key, value));
        size_ += key.size() + value.size();
    }

    /// @brief 添加 Delete 操作
    void BatchDelete(std::string_view key) {
        if (status_ != BatchStatus::kActive) return;

        entries_.emplace_back(BatchEntry::MakeDelete(key));
        size_ += key.size();
    }

    /**
     * @brief 提交批量操作
     * @param memtable 目标 MemTable
     * @return true 成功，false 失败
     */
    bool Commit(std::shared_ptr<MemTable> memtable) {
        if (status_ != BatchStatus::kActive) {
            return false;
        }

        status_ = BatchStatus::kCommitting;

        try {
            // 原子性应用所有操作
            for (const auto& entry : entries_) {
                if (entry.type == BatchEntry::kPut) {
                    memtable->Put(entry.key, entry.value);
                } else {
                    memtable->Delete(entry.key);
                }
            }
            status_ = BatchStatus::kIdle;
            return true;
        } catch (...) {
            status_ = BatchStatus::kRolledBack;
            return false;
        }
    }

    /// @brief 回滚批量操作
    void Rollback() {
        if (status_ == BatchStatus::kActive) {
            status_ = BatchStatus::kRolledBack;
            entries_.clear();
            size_ = 0;
        }
    }

    /// @brief 获取状态
    BatchStatus GetStatus() const { return status_; }

    /// @brief 获取操作数量
    size_t size() const { return entries_.size(); }

    /// @brief 获取数据大小
    size_t data_size() const { return size_; }

    /// @brief 检查是否为空
    bool empty() const { return entries_.empty(); }

    /// @brief 获取操作条目（用于调试）
    const std::vector<BatchEntry>& entries() const { return entries_; }

private:
    BatchStatus status_;
    std::vector<BatchEntry> entries_;
    size_t size_;
};

/**
 * @brief 批量提交管理器
 *
 * 管理多个并发批量提交。
 */
class BatchCommitManager {
public:
    /**
     * @brief 提交配置
     */
    struct Config {
        /// @brief 最大批量大小（字节）
        size_t max_batch_size = 1024 * 1024;  // 1MB

        /// @brief 最大批量条目数
        size_t max_batch_entries = 10000;

        /// @brief 批量提交超时（毫秒）
        uint32_t commit_timeout_ms = 1000;

        /// @brief 是否启用 WAL
        bool enable_wal = true;
    };

    explicit BatchCommitManager(const Config& config = Config())
        : config_(config), next_batch_id_(1) {}

    /**
     * @brief 创建新批量
     * @return 批量 ID
     */
    uint64_t CreateBatch() {
        std::unique_lock<std::mutex> lock(mutex_);
        uint64_t batch_id = next_batch_id_++;
        auto batch = std::make_shared<BatchCommit>();
        batches_[batch_id] = batch;
        return batch_id;
    }

    /**
     * @brief 获取批量
     */
    std::shared_ptr<BatchCommit> GetBatch(uint64_t batch_id) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto it = batches_.find(batch_id);
        return it != batches_.end() ? it->second : nullptr;
    }

    /**
     * @brief 提交批量
     */
    bool CommitBatch(uint64_t batch_id, std::shared_ptr<MemTable> memtable) {
        auto batch = GetBatch(batch_id);
        if (!batch) return false;

        bool success = batch->Commit(std::move(memtable));

        // 清理已完成的批量
        if (success || batch->GetStatus() == BatchStatus::kRolledBack) {
            std::unique_lock<std::mutex> lock(mutex_);
            batches_.erase(batch_id);
        }

        return success;
    }

    /// @brief 获取配置
    const Config& GetConfig() const { return config_; }

    /// @brief 获取活跃批量数
    size_t ActiveBatches() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return batches_.size();
    }

private:
    Config config_;
    std::atomic<uint64_t> next_batch_id_;
    std::unordered_map<uint64_t, std::shared_ptr<BatchCommit>> batches_;
    mutable std::mutex mutex_;
};

/**
 * @brief 批量写入优化器
 *
 * 自动收集小批量写入，合并为大批量提交。
 */
class BatchWriteOptimizer {
public:
    BatchWriteOptimizer(BatchCommitManager& manager, size_t threshold = 4096)
        : manager_(manager), batch_id_(0), buffer_size_(0), threshold_(threshold) {}

    /**
     * @brief 累积写入
     * @param key 键
     * @param value 值
     * @param memtable 目标 MemTable
     * @return true 立即写入，false 累积
     */
    bool AccumulateWrite(std::string_view key, std::string_view value,
                         std::shared_ptr<MemTable> memtable) {
        buffer_size_ += key.size() + value.size();

        if (buffer_size_ >= threshold_) {
            return Flush(memtable);
        }
        return false;
    }

    /**
     * @brief 刷新累积的写入
     * @param memtable 目标 MemTable
     * @return true 已刷新
     */
    bool Flush(std::shared_ptr<MemTable> memtable) {
        if (buffer_size_ == 0) return false;

        memtable->Put(buffer_key_, buffer_value_);
        buffer_key_.clear();
        buffer_value_.clear();
        buffer_size_ = 0;
        return true;
    }

private:
    BatchCommitManager& manager_;
    uint64_t batch_id_;
    size_t buffer_size_;
    size_t threshold_;
    std::string buffer_key_;
    std::string buffer_value_;
};

/**
 * @brief 批量事务
 *
 * RAII 风格的批量提交。
 */
class BatchTransaction {
public:
    explicit BatchTransaction(BatchCommitManager& manager)
        : manager_(manager), batch_id_(manager.CreateBatch()), committed_(false) {
        batch_ = manager.GetBatch(batch_id_);
        batch_->Begin();
    }

    ~BatchTransaction() {
        if (!committed_) {
            Rollback();
        }
    }

    // 禁止拷贝
    BatchTransaction(const BatchTransaction&) = delete;
    BatchTransaction& operator=(const BatchTransaction&) = delete;

    /// @brief 添加 Put 操作
    void Put(std::string_view key, std::string_view value) {
        if (batch_) {
            batch_->BatchPut(key, value);
        }
    }

    /// @brief 添加 Delete 操作
    void Delete(std::string_view key) {
        if (batch_) {
            batch_->BatchDelete(key);
        }
    }

    /**
     * @brief 提交事务
     * @param memtable 目标 MemTable
     * @return true 成功
     */
    bool Commit(std::shared_ptr<MemTable> memtable) {
        if (committed_ || !batch_) return false;

        bool success = batch_->Commit(std::move(memtable));
        committed_ = success;
        return success;
    }

    /// @brief 回滚事务
    void Rollback() {
        if (batch_ && !committed_) {
            batch_->Rollback();
        }
    }

    /// @brief 检查是否已提交
    bool Committed() const { return committed_; }

private:
    BatchCommitManager& manager_;
    uint64_t batch_id_;
    std::shared_ptr<BatchCommit> batch_;
    bool committed_;
};

}  // namespace lsm
}  // namespace mokv
