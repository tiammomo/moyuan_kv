#pragma once
#include <memory>

#include "mokv/config.hpp"
#include "mokv/db.hpp"
#include "mokv/raft/config.hpp"
#include "mokv/raft/pod.hpp"

namespace mokv {

class ResourceManager {
public:
    static ResourceManager& instance() {
        static std::unique_ptr<ResourceManager> instance = std::make_unique<ResourceManager>();
        return *instance;
    }

    ResourceManager() {
        config_manager_ = std::make_unique<raft::ConfigManager>();
        if (!config_manager_->Load()) {
            config_manager_->LoadFromMokvConfig(DefaultConfig());
        }
    }

    DB& db() {
        // db 需要延迟加载，因为 Client 和 service 共用 ResourceManager
        return *db_;
    }

    void InitDb() {
        db_ = std::make_shared<DB>();
    }

    void InitDb(const DBConfig& config) {
        db_ = std::make_shared<DB>(config);
    }

    void InitDb(const MokvConfig& config) {
        DBConfig db_config;
        db_config.data_dir = config.data_dir;
        db_config.memtable_max_size = config.GetMemTableSizeBytes();
        db_config.enable_block_cache = config.block_cache_size_mb > 0;
        db_config.block_cache.max_capacity = config.GetBlockCacheSizeBytes();
        db_ = std::make_shared<DB>(db_config);
    }

    raft::Pod& pod() {
        return *pod_;
    }

    // 一定要先InitDb
    void InitPod() {
        pod_ = std::make_unique<raft::Pod>(config_manager_->local_address().id(), config_manager_->config(), db_);
    }

    bool LoadConfig(const MokvConfig& config) {
        if (!config_manager_) {
            config_manager_ = std::make_unique<raft::ConfigManager>();
        }
        return config_manager_->LoadFromMokvConfig(config);
    }

    bool LoadConfig(const std::filesystem::path& path) {
        if (!config_manager_) {
            config_manager_ = std::make_unique<raft::ConfigManager>();
        }
        return config_manager_->Load(path);
    }

    void ConfigureForTesting(const raft::Config& config, int32_t local_id) {
        MokvConfig mokv_config = DefaultConfig();
        mokv_config.node_id = local_id;
        mokv_config.peers.clear();
        for (const auto& address : config.addresses()) {
            mokv_config.peers.push_back({address.id(), address.ip(), static_cast<uint16_t>(address.port())});
            if (address.id() == local_id) {
                mokv_config.host = address.ip();
                mokv_config.port = static_cast<uint16_t>(address.port());
            }
        }
        LoadConfig(mokv_config);
    }

    raft::ConfigManager& config_manager() {
        return *config_manager_;
    }

    void Load() {
        instance();
    }

    void CloseDb() {
        db_ = nullptr;
    }

    void Close() {
        pod_ = nullptr;
        std::cout << "finish close pod" << std::endl;
        CloseDb();
    }

private:
    std::unique_ptr<raft::ConfigManager> config_manager_;
    std::shared_ptr<DB> db_;
    std::unique_ptr<raft::Pod> pod_;
};

}
