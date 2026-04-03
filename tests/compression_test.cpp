#include <cstdio>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mokv/db.hpp"
#include "mokv/lsm/sst.hpp"
#include "mokv/utils/compression.hpp"

#include <lz4frame.h>

namespace {

std::string KeyFor(int i) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "key-%06d", i);
    return buffer;
}

std::string ValueFor(int i) {
    return "compressible-payload-" + std::string(96, static_cast<char>('a' + (i % 4)));
}

void CleanArtifacts() {
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

mokv::lsm::CompressionConfig TestCompressionConfig() {
    mokv::lsm::CompressionConfig config;
    config.enable = true;
    config.type = mokv::common::CompressionType::kLZ4;
    config.min_size_for_compression = 1;
    return config;
}

mokv::DBConfig TestDBConfig() {
    mokv::DBConfig config;
    config.enable_block_cache = false;
    config.memtable_max_size = 8 * 1024;
    config.compression = TestCompressionConfig();
    return config;
}

class CompressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        CleanArtifacts();
    }

    void TearDown() override {
        CleanArtifacts();
    }
};

struct LZ4DctxDeleter {
    void operator()(LZ4F_dctx* ctx) const {
        if (ctx != nullptr) {
            LZ4F_freeDecompressionContext(ctx);
        }
    }
};

std::string DecompressFrameWithLibLZ4(const char* data, size_t size, size_t expected_size) {
    LZ4F_dctx* raw_ctx = nullptr;
    const auto create_result = LZ4F_createDecompressionContext(&raw_ctx, LZ4F_VERSION);
    if (LZ4F_isError(create_result) || raw_ctx == nullptr) {
        ADD_FAILURE() << LZ4F_getErrorName(create_result);
        return {};
    }
    std::unique_ptr<LZ4F_dctx, LZ4DctxDeleter> ctx(raw_ctx);

    std::string output(expected_size, '\0');
    const char* src = data;
    size_t src_remaining = size;
    char* dst = output.data();
    size_t dst_remaining = output.size();
    bool finished = false;

    while (src_remaining > 0 && dst_remaining > 0) {
        size_t src_size = src_remaining;
        size_t dst_size = dst_remaining;
        const size_t result = LZ4F_decompress(ctx.get(), dst, &dst_size, src, &src_size, nullptr);
        if (LZ4F_isError(result)) {
            ADD_FAILURE() << LZ4F_getErrorName(result);
            return {};
        }
        if (src_size == 0 && dst_size == 0) {
            ADD_FAILURE() << "LZ4 frame decompression made no progress";
            return {};
        }

        src += src_size;
        src_remaining -= src_size;
        dst += dst_size;
        dst_remaining -= dst_size;

        if (result == 0) {
            finished = true;
            break;
        }
    }

    EXPECT_TRUE(finished);
    EXPECT_EQ(dst_remaining, 0U);
    return output;
}

std::vector<char> CompressFrameWithLibLZ4(const std::string& input) {
    LZ4F_preferences_t preferences = LZ4F_INIT_PREFERENCES;
    preferences.frameInfo.blockMode = LZ4F_blockIndependent;
    preferences.frameInfo.contentSize = static_cast<unsigned long long>(input.size());

    std::vector<char> output(LZ4F_compressFrameBound(input.size(), &preferences));
    const size_t compressed_size = LZ4F_compressFrame(
        output.data(), output.size(), input.data(), input.size(), &preferences);
    if (LZ4F_isError(compressed_size)) {
        ADD_FAILURE() << LZ4F_getErrorName(compressed_size);
        return {};
    }
    output.resize(compressed_size);
    return output;
}

}  // namespace

TEST(CompressionDefaults, DBEnablesCompressionByDefault) {
    EXPECT_TRUE(mokv::DBConfig{}.compression.enable);
}

TEST_F(CompressionTest, CompressedSSTRoundTripAfterReload) {
    std::vector<std::pair<std::string, std::string>> storage;
    storage.reserve(128);

    std::vector<mokv::lsm::EntryView> entries;
    entries.reserve(128);
    for (int i = 0; i < 128; ++i) {
        storage.emplace_back(KeyFor(i), ValueFor(i));
        entries.emplace_back(storage.back().first, storage.back().second);
    }

    const auto compression = TestCompressionConfig();
    {
        mokv::lsm::SST sst(entries, 7, compression);
        ASSERT_TRUE(sst.IsCompressed());

        std::string value;
        ASSERT_TRUE(sst.Get(KeyFor(42), value));
        EXPECT_EQ(value, ValueFor(42));
    }

    {
        mokv::lsm::SST sst;
        sst.SetId(7);
        ASSERT_TRUE(sst.Load());
        ASSERT_TRUE(sst.IsCompressed());

        std::string value;
        ASSERT_TRUE(sst.Get(KeyFor(42), value));
        EXPECT_EQ(value, ValueFor(42));
        ASSERT_TRUE(sst.Get(KeyFor(127), value));
        EXPECT_EQ(value, ValueFor(127));
    }
}

TEST_F(CompressionTest, DBReadAfterRestartWithCompression) {
    constexpr int n = 300;
    const auto config = TestDBConfig();

    {
        mokv::DB db(config);
        for (int i = 0; i < n; ++i) {
            db.Put(KeyFor(i), ValueFor(i));
        }
    }

    {
        mokv::DB db(config);
        for (int i = 0; i < n; ++i) {
            std::string value;
            ASSERT_TRUE(db.Get(KeyFor(i), value));
            EXPECT_EQ(value, ValueFor(i));
        }
    }
}

TEST(CompressionInterop, ProjectLZ4PayloadReadableByLibLZ4) {
    const std::string input = "mokv-lz4-frame-" + std::string(512, 'x');
    mokv::common::LZ4Compressor compressor;

    const auto compressed = compressor.Compress(input.data(), input.size());
    ASSERT_FALSE(compressed.empty());

    const std::string output =
        DecompressFrameWithLibLZ4(compressed.data(), compressed.size(), input.size());
    EXPECT_EQ(output, input);
}

TEST(CompressionInterop, ProjectLZ4PayloadReadsExternalLibLZ4Frame) {
    const std::string input = "external-lz4-frame-" + std::string(512, 'y');
    const std::vector<char> frame = CompressFrameWithLibLZ4(input);

    mokv::common::LZ4Compressor compressor;
    std::string output(input.size(), '\0');
    mokv::common::CompressedData compressed(frame.data(), frame.size(), input.size());

    const size_t decompressed_size =
        compressor.Decompress(compressed, output.data(), output.size());
    ASSERT_EQ(decompressed_size, input.size());
    EXPECT_EQ(output, input);
}
