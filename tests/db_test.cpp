#include <filesystem>
#include <gtest/gtest.h>
#include <string>

#include "mokv/db.hpp"

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
    config.memtable_max_size = 64 * 1024 * 1024;
    config.compression.enable = false;
    return config;
}

void PopulateDB(mokv::DB& db, int n) {
    for (int i = 0; i < n; i++) {
        db.Put(std::to_string(i), std::to_string(i + 1));
    }
}

void VerifyDB(mokv::DB& db, int n) {
    for (int i = 0; i < n; i++) {
        std::string value;
        ASSERT_TRUE(db.Get(std::to_string(i), value));
        ASSERT_EQ(value, std::to_string(i + 1));
    }
}

class DBTest : public ::testing::Test {
protected:
    void SetUp() override {
        CleanDBArtifacts();
    }

    void TearDown() override {
        CleanDBArtifacts();
    }
};

}  // namespace

TEST_F(DBTest, Function) {
    mokv::DB db(TestDBConfig());
    const int n = 10000;
    PopulateDB(db, n);
    VerifyDB(db, n);
}

TEST_F(DBTest, ReadAfterRestart) {
    const int n = 5000;
    {
        mokv::DB db(TestDBConfig());
        PopulateDB(db, n);
    }
    {
        mokv::DB db(TestDBConfig());
        VerifyDB(db, n);
    }
}
