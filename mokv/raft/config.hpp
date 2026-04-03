#pragma once
#include <cstdint>
#include <iostream>
#include <fstream>

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
        std::ifstream fin(name_);
        if (!fin.is_open()) {
            return false;
        }
        size_t size;
        fin >> size;
        int32_t id;
        std::string ip;
        int32_t port;
        for (size_t i = 0; i < size; i++) {
            fin >> id;
            fin >> ip;
            fin >> port;
            auto address_ptr = config_.add_addresses();
            address_ptr->set_id(id);
            address_ptr->set_ip(ip);
            address_ptr->set_port(port);
        }

        fin >> id;
        fin >> ip;
        fin >> port;

        local_address_.set_id(id);
        local_address_.set_ip(ip);
        local_address_.set_port(port);
        fin.close();
        ready_ = true;
        return true;
    }


private:
    constexpr static const char* name_ = "raft.cfg";
    bool ready_ = false;
    mokv::raft::Config config_;
    mokv::raft::Address local_address_;
};


}
}