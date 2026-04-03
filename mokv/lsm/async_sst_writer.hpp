/**
 * @file async_sst_writer.hpp
 * @brief 异步 SST 写入器
 *
 * 使用 io_uring 实现异步刷盘：
 * - 异步写入 SST 文件
 * - 批量提交优化
 * - 完成回调处理
 *
 * @section async_write_flow 异步写入流程
 * ┌─────────────────────────────────────────────────────────────────┐
 * │                    异步 SST 写入流程                             │
 * ├─────────────────────────────────────────────────────────────────┤
 * │                                                                 │
 │  1. MemTable 准备就绪                                            │
 *  │        ↓                                                       │
 │  2. 序列化数据到缓冲区                                           │
 *  │        ↓                                                       │
 │  3. 提交异步写入请求                                              │
 *  │        ↓                                                       │
 │  4. 后台处理其他请求                                              │
 *  │        ↓                                                       │
 │  5. 写入完成回调                                                  │
 *  │        ↓                                                       │
 │  6. 更新 Manifest                                                 │
 *                                                                 │
 └─────────────────────────────────────────────────────────────────┘
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>

#include "mokv/lsm/sst.hpp"
#include "mokv/lsm/async_io.hpp"

namespace mokv {
namespace lsm {

/**
 * @brief 异步 SST 写入结果
 */
struct AsyncWriteResult {
    std::shared_ptr<SST> sst;   ///< 写入的 SST
    bool success;               ///< 是否成功
    std::string error_message;  ///< 错误信息
};

/**
 * @brief 异步 SST 写入器
 *
 * 提供异步刷盘功能，将 MemTable 异步写入为 SST 文件。
 */
class AsyncSSTWriter {
public:
    /**
     * @brief 写入配置
     */
    struct Config {
        /// @brief IO 引擎配置
        IOEngine::Config io_config;

        /// @brief 缓冲区池配置
        size_t buffer_size = 256 * 1024;     // 256KB
        size_t buffer_count = 16;

        /// @brief 写入队列深度
        size_t write_queue_depth = 32;

        /// @brief 是否启用压缩
        bool enable_compression = true;

        /// @brief 压缩配置
        CompressionConfig compression{};
    };

    explicit AsyncSSTWriter(const Config& config);
    ~AsyncSSTWriter();

    // 禁止拷贝
    AsyncSSTWriter(const AsyncSSTWriter&) = delete;
    AsyncSSTWriter& operator=(const AsyncSSTWriter&) = delete;

    /**
     * @brief 异步写入 MemTable
     * @param memtable 内存表
     * @param sst_id SST ID
     * @param callback 完成回调
     */
    void WriteAsync(std::shared_ptr<MemTable> memtable, size_t sst_id,
                    std::function<void(const AsyncWriteResult&)> callback = nullptr);

    /**
     * @brief 同步写入 MemTable
     * @param memtable 内存表
     * @param sst_id SST ID
     * @return 写入的 SST
     */
    std::shared_ptr<SST> WriteSync(std::shared_ptr<MemTable> memtable, size_t sst_id);

    /**
     * @brief 等待所有挂起的写入完成
     */
    void Flush();

    /**
     * @brief 获取挂起的写入数
     */
    size_t PendingWrites() const {
        return pending_writes_.load(std::memory_order_relaxed);
    }

    /// @brief 检查是否就绪
    bool IsReady() const {
        return io_engine_ && io_engine_->IsReady();
    }

    /// @brief 获取配置
    const Config& GetConfig() const { return config_; }

    /**
     * @brief 统计信息
     */
    struct Stats {
        std::atomic_size_t total_writes{0};      ///< 总写入数
        std::atomic_size_t successful_writes{0}; ///< 成功写入数
        std::atomic_size_t failed_writes{0};     ///< 失败写入数
        std::atomic_uint64_t total_bytes{0};     ///< 总写入字节数
        std::atomic_uint64_t total_latency_ns{0}; ///< 总延迟（纳秒）

        /// @brief 获取平均延迟（纳秒）
        uint64_t AvgLatencyNs() const {
            size_t count = successful_writes.load();
            return count > 0 ? total_latency_ns.load() / count : 0;
        }

        /// @brief 获取吞吐量（MB/s）
        double ThroughputMBps() const {
            uint64_t bytes = total_bytes.load();
            uint64_t latency = total_latency_ns.load();
            return latency > 0 ? (bytes / 1024.0 / 1024.0) / (latency / 1e9) : 0;
        }
    };

    /// @brief 获取统计信息
    const Stats& GetStats() const { return stats_; }

private:
    void WriterThread();

    Config config_;
    std::unique_ptr<IOEngine> io_engine_;
    std::unique_ptr<IOBufferPool> buffer_pool_;

    std::atomic<bool> running_{true};
    std::thread writer_thread_;

    // 写入队列
    struct WriteRequest {
        std::shared_ptr<MemTable> memtable;
        size_t sst_id;
        std::function<void(const AsyncWriteResult&)> callback;
        uint64_t start_time;
    };
    std::queue<WriteRequest> write_queue_;
    std::mutex write_queue_mutex_;
    std::condition_variable write_cv_;

    std::atomic_size_t pending_writes_{0};
    Stats stats_;
};

/**
 * @brief 批量异步 SST 写入器
 *
 * 优化批量 MemTable 的写入，减少 I/O 操作。
 */
class BatchAsyncSSTWriter {
public:
    BatchAsyncSSTWriter(AsyncSSTWriter& writer, size_t batch_size = 8)
        : writer_(writer), batch_size_(batch_size), current_batch_(0) {}

    /**
     * @brief 添加 MemTable 到批次
     */
    void Add(std::shared_ptr<MemTable> memtable, size_t sst_id) {
        if (batch_.empty()) {
            start_time_ = std::chrono::high_resolution_clock::now();
        }
        batch_.emplace_back(memtable, sst_id);
        ++current_batch_;
    }

    /**
     * @brief 提交批次（异步）
     * @param callback 批次完成回调
     */
    void CommitAsync(std::function<void(size_t, double)> callback = nullptr);

    /**
     * @brief 提交批次（同步）
     * @return 成功写入的 SST 数量
     */
    size_t CommitSync();

    /// @brief 清空批次
    void Clear() {
        batch_.clear();
        current_batch_ = 0;
    }

    /// @brief 获取当前批次大小
    size_t size() const { return batch_.size(); }

    /// @brief 检查批次是否已满
    bool full() const { return batch_.size() >= batch_size_; }

private:
    AsyncSSTWriter& writer_;
    size_t batch_size_;
    size_t current_batch_;
    std::vector<std::pair<std::shared_ptr<MemTable>, size_t>> batch_;
    std::chrono::high_resolution_clock::time_point start_time_;
};

}  // namespace lsm
}  // namespace mokv
