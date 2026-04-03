/**
 * @file kvstore.hpp
 * @brief KV 存储接口抽象层
 *
 * 定义 KV 存储的抽象接口，便于：
 * - 单元测试（使用 Mock 实现）
 * - 实现替换（内存/磁盘/分布式）
 * - API 扩展
 */

#pragma once

#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>

#include "mokv/config.hpp"
#include "mokv/db.hpp"

namespace mokv {

/**
 * @brief KV 存储结果
 */
struct KVResult {
    bool success;                     ///< 操作是否成功
    std::string value;                ///< 获取的值（Get 操作）
    std::string message;              ///< 结果消息
    int32_t leader_id = -1;           ///< 领导者节点 ID（用于重定向）

    static KVResult OK() {
        return KVResult{true, "", "OK", -1};
    }

    static KVResult OK(const std::string& value) {
        return KVResult{true, value, "OK", -1};
    }

    static KVResult Fail(const std::string& message) {
        return KVResult{false, "", message, -1};
    }

    static KVResult NotLeader(int32_t leader_id, const std::string& message = "Not leader") {
        return KVResult{false, "", message, leader_id};
    }
};

/**
 * @brief 异步 KV 存储结果
 */
using KVFuture = std::future<KVResult>;

/**
 * @brief KV 存储接口
 *
 * 提供键值存储的抽象接口，支持：
 * - 同步操作：Get, Put, Delete
 * - 异步操作：GetAsync, PutAsync, DeleteAsync
 * - 批量操作：Batch
 */
class KVStore {
public:
    virtual ~KVStore() = default;

    // ==================== 同步操作 ====================

    /**
     * @brief 获取键对应的值
     * @param key 键
     * @return 结果，包含值（如果存在）
     */
    virtual std::optional<std::string> Get(std::string_view key) = 0;

    /**
     * @brief 存储键值对
     * @param key 键
     * @param value 值
     * @return 操作结果
     */
    virtual KVResult Put(std::string_view key, std::string_view value) = 0;

    /**
     * @brief 删除键
     * @param key 键
     * @return 操作结果
     */
    virtual KVResult Delete(std::string_view key) = 0;

    /**
     * @brief 检查键是否存在
     * @param key 键
     * @return true 存在，false 不存在
     */
    virtual bool Exists(std::string_view key) = 0;

    /**
     * @brief 获取所有键
     * @return 键列表
     */
    virtual std::vector<std::string> ListKeys() = 0;

    /**
     * @brief 获取指定前缀下的键
     * @param prefix 键前缀
     * @return 键列表
     */
    virtual std::vector<std::string> ListKeysByPrefix(std::string_view prefix) = 0;

    /**
     * @brief 获取指定前缀下的键值对
     * @param prefix 键前缀
     * @return 键值对列表
     */
    virtual std::vector<std::pair<std::string, std::string>> ListEntriesByPrefix(std::string_view prefix) = 0;

    /**
     * @brief 清空所有数据
     * @return 操作结果
     */
    virtual KVResult Clear() = 0;

    // ==================== 异步操作 ====================

    /**
     * @brief 异步获取值
     * @param key 键
     * @return 异步结果
     */
    virtual KVFuture GetAsync(std::string_view key) = 0;

    /**
     * @brief 异步存储键值对
     * @param key 键
     * @param value 值
     * @return 异步结果
     */
    virtual KVFuture PutAsync(std::string_view key, std::string_view value) = 0;

    /**
     * @brief 异步删除键
     * @param key 键
     * @return 异步结果
     */
    virtual KVFuture DeleteAsync(std::string_view key) = 0;

    // ==================== 批量操作 ====================

    /**
     * @brief 批量写入
     * @param entries 键值对列表
     * @return 操作结果
     */
    virtual KVResult Batch(const std::vector<std::pair<std::string, std::string>>& entries) = 0;

    /**
     * @brief 批量删除
     * @param keys 键列表
     * @return 操作结果
     */
    virtual KVResult BatchDelete(const std::vector<std::string>& keys) = 0;

    // ==================== 统计信息 ====================

    /**
     * @brief 获取键值对数量
     * @return 数量
     */
    virtual size_t Size() const = 0;

    /**
     * @brief 获取存储大小（字节）
     * @return 大小
     */
    virtual size_t BytesSize() const = 0;

    /**
     * @brief 检查是否为空
     * @return true 为空
     */
    virtual bool Empty() const = 0;

    // ==================== 连接管理 ====================

    /**
     * @brief 健康检查
     * @return true 健康
     */
    virtual bool HealthCheck() = 0;

    /**
     * @brief 关闭存储
     */
    virtual void Close() = 0;
};

class DBKVStore final : public KVStore {
public:
    explicit DBKVStore(const MokvConfig& config = DefaultConfig());
    explicit DBKVStore(DBConfig config);

    std::optional<std::string> Get(std::string_view key) override;
    KVResult Put(std::string_view key, std::string_view value) override;
    KVResult Delete(std::string_view key) override;
    bool Exists(std::string_view key) override;
    std::vector<std::string> ListKeys() override;
    std::vector<std::string> ListKeysByPrefix(std::string_view prefix) override;
    std::vector<std::pair<std::string, std::string>> ListEntriesByPrefix(std::string_view prefix) override;
    KVResult Clear() override;

    KVFuture GetAsync(std::string_view key) override;
    KVFuture PutAsync(std::string_view key, std::string_view value) override;
    KVFuture DeleteAsync(std::string_view key) override;

    KVResult Batch(const std::vector<std::pair<std::string, std::string>>& entries) override;
    KVResult BatchDelete(const std::vector<std::string>& keys) override;

    size_t Size() const override;
    size_t BytesSize() const override;
    bool Empty() const override;

    bool HealthCheck() override;
    void Close() override;

    DB& db();
    const DB& db() const;

private:
    void LoadKeyIndex();
    bool IsReservedKey(std::string_view key) const;

private:
    std::shared_ptr<DB> db_;
    mutable std::mutex metadata_mutex_;
    std::vector<std::string> known_keys_;
    size_t tracked_bytes_size_{0};
    bool closed_{false};
};

/**
 * @brief 工厂方法创建 KV 存储
 * @param config 配置
 * @return KV 存储实例
 */
std::unique_ptr<KVStore> CreateKVStore(const MokvConfig& config);

}  // namespace mokv
