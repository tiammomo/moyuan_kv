#pragma once
#include <memory>

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
        config_manager_->Load();
    }

    DB& db() {
        // db 需要延迟加载，因为 Client 和 service 共用 ResourceManager
        return *db_;
    }

    void InitDb() {
        db_ = std::make_shared<DB>();
    }

    raft::Pod& pod() {
        return *pod_;
    }

    // 一定要先InitDb
    void InitPod() {
        pod_ = std::make_unique<raft::Pod>(config_manager_->local_address().id(), config_manager_->config(), db_);
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