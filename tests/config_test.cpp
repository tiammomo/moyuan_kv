#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "mokv/config.hpp"
#include "mokv/raft/config.hpp"

namespace {

std::filesystem::path WriteConfigFile(const std::string& name, const std::string& content) {
    const auto path = std::filesystem::current_path() / name;
    std::ofstream output(path);
    output << content;
    output.close();
    return path;
}

}  // namespace

TEST(ConfigTest, LoadsLegacyRaftConfigIntoMokvConfig) {
    const auto path = WriteConfigFile(
        "legacy-raft.cfg",
        "2\n"
        "1 127.0.0.1 9001\n"
        "2 127.0.0.1 9002\n"
        "2 127.0.0.1 9002\n");

    mokv::MokvConfig config;
    ASSERT_TRUE(config.LoadFromFile(path));
    EXPECT_EQ(config.node_id, 2);
    EXPECT_EQ(config.host, "127.0.0.1");
    EXPECT_EQ(config.port, 9002);
    ASSERT_EQ(config.peers.size(), 2u);
    ASSERT_NE(config.FindPeer(2), nullptr);
    EXPECT_EQ(config.FindPeer(2)->port, 9002);
}

TEST(ConfigTest, RoundTripsModernConfigAndFeedsConfigManager) {
    mokv::MokvConfig config = mokv::DefaultConfig();
    config.host = "127.0.0.1";
    config.port = 9102;
    config.node_id = 2;
    config.data_dir = "./data-test";
    config.block_cache_size_mb = 64;
    config.memtable_size_mb = 16;
    config.peers = {
        {1, "127.0.0.1", 9101},
        {2, "127.0.0.1", 9102},
    };

    const auto path = std::filesystem::current_path() / "mokv.conf";
    ASSERT_TRUE(config.SaveToFile(path));

    mokv::MokvConfig loaded;
    ASSERT_TRUE(loaded.LoadFromFile(path));
    EXPECT_EQ(loaded.node_id, 2);
    EXPECT_EQ(loaded.host, "127.0.0.1");
    EXPECT_EQ(loaded.port, 9102);
    EXPECT_EQ(loaded.block_cache_size_mb, 64u);
    EXPECT_EQ(loaded.memtable_size_mb, 16u);
    ASSERT_EQ(loaded.peers.size(), 2u);

    mokv::raft::ConfigManager manager;
    ASSERT_TRUE(manager.Load(path));
    EXPECT_EQ(manager.local_address().id(), 2);
    EXPECT_EQ(manager.local_address().port(), 9102);
    EXPECT_EQ(manager.config().addresses_size(), 2);
}
