/**
 * @file config.hpp
 * @brief MoKV 配置类
 *
 * 提供统一的配置管理，支持：
 * - 服务器端口配置
 * - 内存限制配置
 * - Raft 协议配置
 * - 存储路径配置
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mokv {

/**
 * @brief mokv 服务器配置
 */
struct MokvConfig {
    struct RaftPeer {
        int32_t id = 0;
        std::string host = "127.0.0.1";
        uint16_t port = 0;
    };

    // ============ 服务器配置 ============
    uint16_t port = 9001;                    ///< 服务端口
    std::string host = "0.0.0.0";            ///< 监听地址

    // ============ 存储配置 ============
    std::filesystem::path data_dir = "./data";  ///< 数据目录
    size_t max_memory_mb = 4096;             ///< 最大内存使用（MB）
    size_t block_cache_size_mb = 256;        ///< 块缓存大小（MB）
    size_t memtable_size_mb = 64;            ///< MemTable 大小（MB）

    // ============ Raft 配置 ============
    int32_t node_id = 1;                     ///< 节点 ID
    int32_t election_timeout_ms = 5000;      ///< 选举超时时间（毫秒）
    int32_t heartbeat_interval_ms = 1000;    ///< 心跳间隔（毫秒）
    int32_t snapshot_interval_s = 3600;      ///< 快照间隔（秒）
    std::vector<RaftPeer> peers;             ///< Raft 节点列表

    // ============ Compaction 配置 ============
    bool enable_compaction = true;           ///< 是否启用自动压缩
    size_t level0_compaction_threshold = 4;  ///< Level 0 SST 文件数量阈值

    // ============ 性能配置 ============
    uint32_t background_threads = 4;         ///< 后台线程数量
    uint32_t max_background_jobs = 8;        ///< 最大后台任务数

    // ============ 日志配置 ============
    bool verbose_logging = false;            ///< 详细日志
    std::string log_level = "INFO";          ///< 日志级别

    /**
     * @brief 验证配置有效性
     * @return true 配置有效，false 配置无效
     */
    bool Validate() const {
        if (port == 0) {
            return false;
        }
        if (max_memory_mb == 0) {
            return false;
        }
        if (election_timeout_ms < 1000) {
            return false;
        }
        if (heartbeat_interval_ms <= 0) {
            return false;
        }
        for (const auto& peer : peers) {
            if (peer.id <= 0 || peer.host.empty() || peer.port == 0) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 获取内存限制（字节）
     * @return 内存限制（字节）
     */
    size_t GetMaxMemoryBytes() const {
        return max_memory_mb * 1024 * 1024;
    }

    /**
     * @brief 获取块缓存大小（字节）
     * @return 块缓存大小（字节）
     */
    size_t GetBlockCacheSizeBytes() const {
        return block_cache_size_mb * 1024 * 1024;
    }

    /**
     * @brief 获取 MemTable 大小（字节）
     * @return MemTable 大小（字节）
     */
    size_t GetMemTableSizeBytes() const {
        return memtable_size_mb * 1024 * 1024;
    }

    /**
     * @brief 从文件加载配置
     * @param path 配置文件路径
     * @return true 加载成功，false 加载失败
     */
    bool LoadFromFile(const std::filesystem::path& path);

    /**
     * @brief 保存配置到文件
     * @param path 配置文件路径
     * @return true 保存成功，false 保存失败
     */
    bool SaveToFile(const std::filesystem::path& path) const;

    /**
     * @brief 从环境变量加载配置
     *
     * 支持的环境变量：
     * - MOKV_PORT: 服务端口
     * - MOKV_DATA_DIR: 数据目录
     * - MOKV_MAX_MEMORY: 最大内存（MB）
     * - MOKV_PEERS: 以 `id,host,port;...` 格式指定 Raft 节点
     */
    void LoadFromEnv();

    const RaftPeer* FindPeer(int32_t id) const;
};

/**
 * @brief 默认配置
 * @return 默认配置实例
 */
using MoKVConfig = MokvConfig;

inline MokvConfig DefaultConfig() {
    MokvConfig config;
    config.peers.push_back({config.node_id, config.host, config.port});
    return config;
}

}  // namespace mokv
