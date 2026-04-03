#include <filesystem>

#include <gtest/gtest.h>

#include "mokv/raft/service.hpp"

namespace {

void CleanDBArtifacts() {
    namespace fs = std::filesystem;

    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (ec) {
        return;
    }

    fs::remove(cwd / "manifest", ec);
    ec.clear();
    fs::remove(cwd / "raft_log_meta", ec);
    ec.clear();

    for (const auto& entry : fs::directory_iterator(cwd, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        if (entry.path().extension() == ".sst") {
            fs::remove(entry.path(), ec);
            ec.clear();
        }
    }
}

mokv::DBConfig TestDBConfig() {
    mokv::DBConfig config;
    config.enable_block_cache = false;
    config.memtable_max_size = 8 * 1024;
    config.compression.enable = false;
    return config;
}

mokv::raft::Config MakeSingleNodeConfig() {
    mokv::raft::Config config;
    auto* local = config.add_addresses();
    local->set_id(1);
    local->set_ip("127.0.0.1");
    local->set_port(19091);
    return config;
}

mokv::raft::Config MakeTwoNodeConfig() {
    mokv::raft::Config config;

    auto* local = config.add_addresses();
    local->set_id(1);
    local->set_ip("127.0.0.1");
    local->set_port(19091);

    auto* leader = config.add_addresses();
    leader->set_id(2);
    leader->set_ip("127.0.0.1");
    leader->set_port(19092);

    return config;
}

void InitServiceState(const mokv::raft::Config& config, int32_t local_id) {
    auto& resources = mokv::ResourceManager::instance();
    resources.Close();
    resources.ConfigureForTesting(config, local_id);
    resources.InitDb(TestDBConfig());
    resources.InitPod();
}

class RpcServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        CleanDBArtifacts();
        mokv::ResourceManager::instance().Close();
    }

    void TearDown() override {
        mokv::ResourceManager::instance().Close();
        CleanDBArtifacts();
    }
};

}  // namespace

TEST_F(RpcServiceTest, PutAndGetReturnExplicitSuccessCodes) {
    InitServiceState(MakeSingleNodeConfig(), 1);
    mokv::ResourceManager::instance().pod().SetStateForTesting(mokv::raft::PodStatus::Leader, 1);

    mokv::MokvServiceImpl service;

    grpc::ServerContext put_ctx;
    mokv::raft::PutReq put_req;
    mokv::raft::PutRsp put_rsp;
    put_req.set_key("alpha");
    put_req.set_value("beta");

    const auto put_status = service.Put(&put_ctx, &put_req, &put_rsp);
    ASSERT_TRUE(put_status.ok());
    ASSERT_TRUE(put_rsp.has_base());
    EXPECT_EQ(put_rsp.base().code(), 0);

    grpc::ServerContext get_ctx;
    mokv::raft::GetReq get_req;
    mokv::raft::GetRsp get_rsp;
    get_req.set_key("alpha");

    const auto get_status = service.Get(&get_ctx, &get_req, &get_rsp);
    ASSERT_TRUE(get_status.ok());
    ASSERT_TRUE(get_rsp.has_base());
    EXPECT_EQ(get_rsp.base().code(), 0);
    EXPECT_EQ(get_rsp.value(), "beta");
}

TEST_F(RpcServiceTest, GetMissingKeyReturnsNotFoundCode) {
    InitServiceState(MakeSingleNodeConfig(), 1);
    mokv::ResourceManager::instance().pod().SetStateForTesting(mokv::raft::PodStatus::Leader, 1);

    mokv::MokvServiceImpl service;

    grpc::ServerContext get_ctx;
    mokv::raft::GetReq get_req;
    mokv::raft::GetRsp get_rsp;
    get_req.set_key("missing");

    const auto get_status = service.Get(&get_ctx, &get_req, &get_rsp);
    ASSERT_TRUE(get_status.ok());
    ASSERT_TRUE(get_rsp.has_base());
    EXPECT_EQ(get_rsp.base().code(), 1);
    EXPECT_TRUE(get_rsp.value().empty());
}

TEST_F(RpcServiceTest, PutOnFollowerReturnsRedirectWithLeaderAddress) {
    InitServiceState(MakeTwoNodeConfig(), 1);
    mokv::ResourceManager::instance().pod().SetStateForTesting(mokv::raft::PodStatus::Follower, 2);

    mokv::MokvServiceImpl service;

    grpc::ServerContext put_ctx;
    mokv::raft::PutReq put_req;
    mokv::raft::PutRsp put_rsp;
    put_req.set_key("alpha");
    put_req.set_value("beta");

    const auto put_status = service.Put(&put_ctx, &put_req, &put_rsp);
    ASSERT_TRUE(put_status.ok());
    ASSERT_TRUE(put_rsp.has_base());
    EXPECT_EQ(put_rsp.base().code(), -2);
    ASSERT_TRUE(put_rsp.has_leader_addr());
    EXPECT_EQ(put_rsp.leader_addr().id(), 2);
    EXPECT_EQ(put_rsp.leader_addr().port(), 19092);
}

TEST_F(RpcServiceTest, ReadFromLeaderOnFollowerReturnsRedirectWithLeaderAddress) {
    InitServiceState(MakeTwoNodeConfig(), 1);
    mokv::ResourceManager::instance().pod().SetStateForTesting(mokv::raft::PodStatus::Follower, 2);

    mokv::MokvServiceImpl service;

    grpc::ServerContext get_ctx;
    mokv::raft::GetReq get_req;
    mokv::raft::GetRsp get_rsp;
    get_req.set_key("alpha");
    get_req.set_read_from_leader(true);

    const auto get_status = service.Get(&get_ctx, &get_req, &get_rsp);
    ASSERT_TRUE(get_status.ok());
    ASSERT_TRUE(get_rsp.has_base());
    EXPECT_EQ(get_rsp.base().code(), -2);
    ASSERT_TRUE(get_rsp.has_leader_addr());
    EXPECT_EQ(get_rsp.leader_addr().id(), 2);
    EXPECT_EQ(get_rsp.leader_addr().port(), 19092);
}
