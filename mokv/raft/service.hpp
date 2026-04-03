#pragma once

#include "mokv/resource_manager.hpp"
#include "mokv/raft/protos/raft.grpc.pb.h"

#include <grpcpp/support/status.h>
#include <iostream>
#include <string>

namespace mokv {

class MokvServiceImpl final : public raft::MoKVService::Service {
public:
    using Status = grpc::Status;

    Status Put(grpc::ServerContext* ctx,
               const raft::PutReq* req,
               raft::PutRsp* rsp) override {
        (void)ctx;
        ResourceManager::instance().pod().Put(*req, *rsp);
        return Status::OK;
    }

    Status Get(grpc::ServerContext* ctx,
               const raft::GetReq* req,
               raft::GetRsp* rsp) override {
        (void)ctx;
        ResourceManager::instance().pod().Get(req, rsp);
        return Status::OK;
    }

    Status UpdateConfig(grpc::ServerContext* ctx,
                        const raft::Config* req,
                        raft::UpdateConfigRsp* rsp) override {
        (void)ctx;
        (void)req;
        (void)rsp;
        return Status::CANCELLED;
    }

    Status RequestVote(grpc::ServerContext* ctx,
                       const raft::RequestVoteReq* req,
                       raft::RequestVoteRsp* rsp) override {
        (void)ctx;
        std::cout << "solve request vote" << std::endl;
        const bool voted = ResourceManager::instance().pod().Vote(*req);
        std::cout << "out " << voted << std::endl;
        auto* base = new raft::Base;
        base->set_code(voted ? 0 : -1);
        rsp->set_allocated_base(base);
        return Status::OK;
    }

    Status Append(grpc::ServerContext* ctx,
                  const raft::AppendReq* req,
                  raft::AppendRsp* rsp) override {
        (void)ctx;
        auto code = ResourceManager::instance().pod().SolveAppend(*req);
        auto base = new raft::Base;
        base->set_code(code);
        rsp->set_allocated_base(base);
        return Status::OK;
    }

};

using MoKVServiceImpl = MokvServiceImpl;

}  // namespace mokv

/*
rpc Put(PutReq) returns (PutRsp) {}
    rpc Get(GetReq) returns (GetRsp) {}
    rpc UpdateConfig(Config) returns (UpdateConfigRsp) {}
    rpc Election(ElectionReq) returns (ElectionRsp) {}
    rpc Append(AppendReq) returns (AppendRsp) {}
*/
