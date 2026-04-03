/**
 * @file read_quorum.hpp
 * @brief 读 Quorum 实现 - 线性化读优化
 *
 * 提供强一致性读取保证：
 * - 多版本验证
 * - 读取时间戳排序
 * - 版本链追踪
 *
 * @section quorum_read_flow Quorum 读取流程
 * ┌─────────────────────────────────────────────────────────────────┐
 * │                    Quorum 读取流程                              │
 * ├─────────────────────────────────────────────────────────────────┤
 * │                                                                 │
 *  1. 读取请求到达                                                   │
 *  │        ↓                                                       │
 *  2. 获取当前读时间戳                                               │
 *  │        ↓                                                       │
 *  3. 从多个副本/版本读取                                            │
 *  │        ↓                                                       │
 *  4. 验证一致性（时间戳比较）                                       │
 *  │        ↓                                                       │
 *  5. 返回最新版本                                                   │
 *                                                                 │
 └─────────────────────────────────────────────────────────────────┘
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mokv {
namespace lsm {

/**
 * @brief 版本信息
 */
struct Version {
    uint64_t version_id;      ///< 版本号
    uint64_t timestamp;       ///< 时间戳
    std::string value;        ///< 值

    Version(uint64_t id, uint64_t ts, std::string_view val)
        : version_id(id), timestamp(ts), value(val) {}
};

/**
 * @brief 读 Quorum 配置
 */
struct ReadQuorumConfig {
    /// @brief 副本数量
    size_t replica_count = 3;

    /// @brief 读取所需的最小副本数
    size_t read_quorum = 2;

    /// @brief 写入所需的最小副本数
    size_t write_quorum = 2;

    /// @brief 版本保留数量
    size_t max_versions = 10;

    /// @brief 是否启用版本链
    bool enable_version_chain = true;

    /// @brief 版本过期时间（毫秒）
    uint64_t version_expiry_ms = 60000;
};

/**
 * @brief 版本管理器
 *
 * 管理键的多个版本，支持版本历史查询。
 */
class VersionManager {
public:
    explicit VersionManager(const ReadQuorumConfig& config = ReadQuorumConfig())
        : config_(config), next_version_id_(1) {}

    /**
     * @brief 添加新版本
     * @param key 键
     * @param value 值
     * @return 版本号
     */
    uint64_t AddVersion(std::string_view key, std::string_view value) {
        uint64_t version_id = next_version_id_++;
        uint64_t timestamp = GetCurrentTimestamp();

        std::unique_lock<std::mutex> lock(mutex_);
        auto& versions = versions_[std::string(key)];
        versions.emplace_back(version_id, timestamp, value);

        // 清理旧版本
        while (versions.size() > config_.max_versions) {
            versions.erase(versions.begin());
        }

        // 更新最大时间戳
        max_timestamp_[std::string(key)] = timestamp;

        return version_id;
    }

    /**
     * @brief 获取键的所有版本
     */
    std::vector<Version> GetVersions(std::string_view key) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto it = versions_.find(std::string(key));
        if (it != versions_.end()) {
            return it->second;
        }
        return {};
    }

    /**
     * @brief 获取最新版本
     */
    std::optional<Version> GetLatestVersion(std::string_view key) {
        auto versions = GetVersions(key);
        if (versions.empty()) return std::nullopt;
        return versions.back();
    }

    /**
     * @brief 获取指定版本
     */
    std::optional<Version> GetVersion(std::string_view key, uint64_t version_id) {
        auto versions = GetVersions(key);
        for (const auto& v : versions) {
            if (v.version_id == version_id) {
                return v;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief 删除键的所有版本
     */
    void DeleteKey(std::string_view key) {
        std::unique_lock<std::mutex> lock(mutex_);
        versions_.erase(std::string(key));
        max_timestamp_.erase(std::string(key));
    }

    /// @brief 获取键的数量
    size_t Size() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return versions_.size();
    }

private:
    static uint64_t GetCurrentTimestamp() {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
    }

    ReadQuorumConfig config_;
    std::atomic<uint64_t> next_version_id_;

    std::unordered_map<std::string, std::vector<Version>> versions_;
    std::unordered_map<std::string, uint64_t> max_timestamp_;
    mutable std::mutex mutex_;
};

/**
 * @brief 读 Quorum 客户端
 *
 * 提供一致性读取接口，支持从多个副本读取并验证。
 */
class ReadQuorum {
public:
    ReadQuorum() : read_count_(0), read_latency_ns_(0) {}

    /**
     * @brief 读取键的值
     * @param key 键
     * @param read_func 读取函数 (key, replica_id) -> optional<value, version>
     * @return 读取结果
     */
    struct ReadResult {
        std::string value;
        uint64_t version_id;
        uint64_t timestamp;
        size_t replicas_read;
        bool is_strong_consistent;
    };

    template<typename ReadFunc>
    ReadResult Read(std::string_view key, ReadFunc read_func) {
        auto start_time = std::chrono::high_resolution_clock::now();

        ReadResult result;
        result.replicas_read = 0;
        result.is_strong_consistent = true;

        // 从所有副本读取
        std::vector<std::pair<std::string, uint64_t>> values;  // (value, version_id)

        for (size_t i = 0; i < config_.replica_count; ++i) {
            auto read_result = read_func(key, i);
            if (read_result.has_value()) {
                values.emplace_back(read_result->value, read_result->version_id);
                result.replicas_read++;
            }
        }

        // 如果没有读取到任何副本，返回空
        if (values.empty()) {
            return result;
        }

        // 查找最新版本（版本号最大）
        uint64_t max_version = 0;
        size_t max_version_count = 0;

        for (const auto& [value, version_id] : values) {
            if (version_id > max_version) {
                max_version = version_id;
                max_version_count = 1;
                result.value = value;
            } else if (version_id == max_version) {
                max_version_count++;
            }
        }

        result.version_id = max_version;

        // 检查强一致性：所有读取到最新版本的副本应该有相同的值
        if (max_version_count < config_.read_quorum) {
            result.is_strong_consistent = false;
        }

        // 统计
        auto end_time = std::chrono::high_resolution_clock::now();
        auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time - start_time).count();

        read_count_++;
        read_latency_ns_ += latency_ns;

        return result;
    }

    /**
     * @brief 线性化读取
     * @param key 键
     * @param read_func 读取函数
     * @return 读取结果（强一致）
     */
    template<typename ReadFunc>
    std::optional<ReadResult> LinearizableRead(std::string_view key, ReadFunc read_func) {
        // 先读取所有副本
        auto result = Read(key, read_func);

        // 检查是否满足 Quorum
        if (result.replicas_read < config_.read_quorum) {
            return std::nullopt;  // 无法满足 Quorum
        }

        // 验证强一致性
        if (!result.is_strong_consistent) {
            // 再次读取以确保一致性
            return LinearizableRead(key, read_func);
        }

        return result;
    }

    /// @brief 获取读取统计
    struct Stats {
        size_t total_reads{0};
        uint64_t total_latency_ns{0};

        uint64_t AvgLatencyNs() const {
            return total_reads > 0 ? total_latency_ns / total_reads : 0;
        }
    };

    Stats GetStats() const {
        Stats stats;
        stats.total_reads = read_count_.load();
        stats.total_latency_ns = read_latency_ns_.load();
        return stats;
    }

    /// @brief 重置统计
    void ResetStats() {
        read_count_ = 0;
        read_latency_ns_ = 0;
    }

    /// @brief 设置配置
    void SetConfig(const ReadQuorumConfig& config) {
        config_ = config;
    }

    /// @brief 获取配置
    const ReadQuorumConfig& GetConfig() const { return config_; }

private:
    ReadQuorumConfig config_;
    std::atomic_size_t read_count_;
    std::atomic_uint64_t read_latency_ns_;
};

/**
 * @brief 快照读取
 *
 * 提供时间点一致的读取视图。
 */
class SnapshotRead {
public:
    /**
     * @brief 快照
     */
    struct Snapshot {
        uint64_t timestamp;
        uint64_t version_id;
        std::unordered_map<std::string, std::string> data;
    };

    /// @brief 创建快照
    uint64_t CreateSnapshot(VersionManager& version_manager) {
        uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();

        snapshots_[timestamp] = timestamp;
        return timestamp;
    }

    /**
     * @brief 从快照读取
     * @param key 键
     * @param snapshot_id 快照 ID
     * @param version_manager 版本管理器
     * @return 值（不存在返回空）
     */
    std::optional<std::string> ReadFromSnapshot(std::string_view key, uint64_t snapshot_id,
                                                 VersionManager& version_manager) {
        // 检查快照是否存在
        if (snapshots_.find(snapshot_id) == snapshots_.end()) {
            return std::nullopt;
        }

        // 获取该时间点的版本
        auto versions = version_manager.GetVersions(key);
        for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
            if (it->timestamp <= snapshot_id) {
                return it->value;
            }
        }

        return std::nullopt;
    }

    /// @brief 删除快照
    void DeleteSnapshot(uint64_t snapshot_id) {
        snapshots_.erase(snapshot_id);
    }

    /// @brief 获取快照数量
    size_t SnapshotCount() const { return snapshots_.size(); }

private:
    std::unordered_set<uint64_t> snapshots_;
};

/**
 * @brief 线性化读优化器
 *
 * 通过版本验证优化读取性能。
 */
class LinearReadOptimizer {
public:
    LinearReadOptimizer(ReadQuorum& quorum, VersionManager& versions)
        : quorum_(quorum), versions_(versions) {}

    /**
     * @brief 优化读取
     * @param key 键
     * @param read_func 读取函数
     * @return 读取结果
     */
    template<typename ReadFunc>
    std::optional<ReadQuorum::ReadResult> OptimizedRead(std::string_view key, ReadFunc read_func) {
        // 首先尝试从缓存读取最新版本
        auto latest = versions_.GetLatestVersion(key);
        if (latest.has_value()) {
            // 验证缓存版本是否仍然有效
            auto result = quorum_.LinearizableRead(key,
                [&](std::string_view k, size_t replica_id) {
                    return read_func(k, replica_id);
                });

            if (result.has_value() && result->version_id == latest->version_id) {
                return result;
            }
        }

        // 需要重新读取
        return quorum_.LinearizableRead(key,
            [&](std::string_view k, size_t replica_id) {
                return read_func(k, replica_id);
            });
    }

private:
    ReadQuorum& quorum_;
    VersionManager& versions_;
};

}  // namespace lsm
}  // namespace mokv
