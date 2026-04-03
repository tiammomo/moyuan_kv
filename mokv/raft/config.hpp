#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "mokv/config.hpp"
#include "mokv/raft/protos/raft.grpc.pb.h"

namespace mokv {
namespace raft {

/*
config |size()|(ip|port)|
*/
class ConfigManager {
public:
    const mokv::raft::Config& config() const {
        return config_;
    }

    const mokv::raft::Address& local_address() const {
        return local_address_;
    }

    bool Load() {
        return Load(DefaultPath());
    }

    bool Load(const std::filesystem::path& path) {
        MokvConfig config = DefaultConfig();
        if (!config.LoadFromFile(path)) {
            return false;
        }
        return LoadFromMokvConfig(config);
    }

    bool LoadFromMokvConfig(const MokvConfig& config) {
        config_.Clear();
        local_address_.Clear();
        ready_ = false;

        for (const auto& peer : config.peers) {
            auto* address = config_.add_addresses();
            address->set_id(peer.id);
            address->set_ip(peer.host);
            address->set_port(peer.port);
            if (peer.id == config.node_id) {
                local_address_ = *address;
            }
        }

        if (local_address_.ip().empty() || local_address_.port() == 0) {
            local_address_.set_id(config.node_id);
            local_address_.set_ip(config.host);
            local_address_.set_port(config.port);
            auto* address = config_.add_addresses();
            address->set_id(config.node_id);
            address->set_ip(config.host);
            address->set_port(config.port);
        }

        ready_ = local_address_.port() > 0;
        return ready_;
    }

    static constexpr const char* DefaultPath() {
        return "raft.cfg";
    }


private:
    bool ready_ = false;
    mokv::raft::Config config_;
    mokv::raft::Address local_address_;
};


}
}
