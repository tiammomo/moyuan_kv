#pragma once

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "mokv/config.hpp"
#include "mokv/raft/client.hpp"
#include "mokv/raft/config.hpp"

namespace mokv {

class DBClient {
public:
    DBClient() {
        MokvConfig config = DefaultConfig();
        const std::filesystem::path default_path = raft::ConfigManager::DefaultPath();
        if (std::filesystem::exists(default_path)) {
            if (!config.LoadFromFile(default_path)) {
                throw std::runtime_error("failed to load config file: " + default_path.string());
            }
        }
        Init(config);
    }

    explicit DBClient(const MokvConfig& config) {
        Init(config);
    }

    explicit DBClient(const std::filesystem::path& config_path) {
        MokvConfig config = DefaultConfig();
        if (!config.LoadFromFile(config_path)) {
            throw std::runtime_error("failed to load config file: " + config_path.string());
        }
        Init(config);
    }

    bool Get(std::string_view key, std::string& value) {
        raft::GetReq req;
        raft::GetRsp rsp;
        grpc::ClientContext ctx;
        gpr_timespec timespec;
        timespec.tv_sec = 2;
        timespec.tv_nsec = 0;
        timespec.clock_type = GPR_TIMESPAN;
        ctx.set_deadline(timespec);
        req.set_key(std::string(key));
        for (auto& client : all_pod_) {
            auto status = client->stub().Get(&ctx, req, &rsp);
            if (status.ok()) {
                if (rsp.base().code() == 0) {
                    value = rsp.value();
                    return true;
                }
            }
        }
        return false;
    }
    
private:
    void Init(const MokvConfig& config) {
        if (!config_manager_.LoadFromMokvConfig(config)) {
            throw std::runtime_error("invalid mokv config for DBClient");
        }
        all_pod_.clear();
        for (auto addr : config_manager_.config().addresses()) {
            auto client = std::make_shared<Client>();
            client->SetIp(addr.ip());
            client->SetPort(addr.port());
            client->Connect();
            all_pod_.emplace_back(client);
        }
    }

public:
    bool Get(std::string_view key, std::string& value, size_t index) {
        {
            raft::GetReq req;
            raft::GetRsp rsp;
            grpc::ClientContext ctx;
            gpr_timespec timespec;
            timespec.tv_sec = 2;
            timespec.tv_nsec = 0;
            timespec.clock_type = GPR_TIMESPAN;
            ctx.set_deadline(timespec);
            req.set_key(std::string(key));
            auto& client = all_pod_[index];
            auto status = client->stub().Get(&ctx, req, &rsp);
            if (status.ok()) {
                if (rsp.base().code() == 0) {
                    value = rsp.value();
                    return true;
                }
            } else {
                client->Reset();
            }
        }
        return false;
    }

    bool SyncGet(std::string_view key, std::string& value) {
        for (auto& client : all_pod_) {
            raft::GetReq req;
            raft::GetRsp rsp;
            grpc::ClientContext ctx;
            gpr_timespec timespec;
            timespec.tv_sec = 2;
            timespec.tv_nsec = 0;
            timespec.clock_type = GPR_TIMESPAN;
            ctx.set_deadline(timespec);
            req.set_key(std::string(key));
            req.set_read_from_leader(true);
            auto status = client->stub().Get(&ctx, req, &rsp);
            if (status.ok()) {
                if (rsp.base().code() == 0) {
                    value = rsp.value();
                    return true;
                } else if (rsp.base().code() == -2) {
                    std::cout << "redirect to leader " << rsp.leader_addr().port() << " " << rsp.leader_addr().id() << std::endl;
                    for (auto& s_client : all_pod_) {
                        if (rsp.leader_addr().ip() == s_client->ip() && rsp.leader_addr().port() == s_client->port()) {
                            raft::GetReq req;
                            raft::GetRsp rsp;
                            grpc::ClientContext ctx;
                            gpr_timespec timespec;
                            timespec.tv_sec = 10;
                            timespec.tv_nsec = 0;
                            timespec.clock_type = GPR_TIMESPAN;
                            ctx.set_deadline(timespec);
                            req.set_key(std::string(key));
                            req.set_read_from_leader(true);
                            auto status = s_client->stub().Get(&ctx, req, &rsp);
                            if (status.ok()) {
                                if (rsp.base().code() == 0) {
                                    value = rsp.value();
                                    return true;
                                } else {
                                    std::cout << "query leader failed, code = " << rsp.base().code() << std::endl;
                                }
                            } else {
                                std::cout << "query leader failed" << std::endl;
                                client->Reset();
                            }
                        }
                    }
                    break;
                }
            }
        }
        return false;
    }
    bool Put(const std::string& key, const std::string& value) {
        for (auto& client : all_pod_) {
            raft::PutReq req;
            raft::PutRsp rsp;
            grpc::ClientContext ctx;
            gpr_timespec timespec;
            timespec.tv_sec = 2;
            timespec.tv_nsec = 0;
            timespec.clock_type = GPR_TIMESPAN;
            ctx.set_deadline(timespec);
            req.set_key(key);
            req.set_value(value);
            auto status = client->stub().Put(&ctx, req, &rsp);
            if (status.ok()) {
                if (rsp.base().code() == 0) {
                    return true;
                } else if (rsp.base().code() == -2) {
                    std::cout << "redirect to leader " << rsp.leader_addr().port() << " " << rsp.leader_addr().id() << std::endl;
                    for (auto& s_client : all_pod_) {
                        if (rsp.leader_addr().ip() == s_client->ip() && rsp.leader_addr().port() == s_client->port()) {
                            raft::PutReq req;
                            raft::PutRsp rsp;
                            grpc::ClientContext ctx;
                            gpr_timespec timespec;
                            timespec.tv_sec = 10;
                            timespec.tv_nsec = 0;
                            timespec.clock_type = GPR_TIMESPAN;
                            ctx.set_deadline(timespec);
                            req.set_key(key);
                            req.set_value(value);
                            auto status = s_client->stub().Put(&ctx, req, &rsp);
                            if (status.ok()) {
                                if (rsp.base().code() == 0) {
                                    return true;
                                } else {
                                    // std::cout << "put leader failed, code = " << rsp.base().code() << std::endl;
                                }
                            } else {
                                // std::cout << "put leader rpc failed" << std::endl;
                                client->Reset();
                            }
                        }
                    }
                    break;
                }
            }
        }
        return false;
    }
private:
    raft::ConfigManager config_manager_;
    std::vector<std::shared_ptr<Client> > all_pod_; 
};

}
