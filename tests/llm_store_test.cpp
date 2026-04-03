#include <filesystem>
#include <gtest/gtest.h>

#include "mokv/llm/store.hpp"

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
    config.memtable_max_size = 8 * 1024 * 1024;
    config.compression.enable = false;
    return config;
}

class LLMStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        CleanDBArtifacts();
    }

    void TearDown() override {
        CleanDBArtifacts();
    }
};

}  // namespace

TEST_F(LLMStoreTest, PromptCacheRoundTripAndListByPrefix) {
    mokv::DBKVStore store(TestDBConfig());
    mokv::llm::LLMStore llm_store(store);

    mokv::llm::PromptCacheEntry entry;
    entry.tenant = "tenant-a";
    entry.app_id = "agent-gateway";
    entry.model = "gpt-5.4";
    entry.prompt_hash = "hash-001";
    entry.response = "{\"answer\":\"ok\"}";
    entry.cached_at_ms = 1712345678;
    entry.input_tokens = 42;
    entry.output_tokens = 18;

    ASSERT_TRUE(llm_store.PutPromptCache(entry).success);

    const auto loaded = llm_store.GetPromptCache("tenant-a", "agent-gateway", "gpt-5.4", "hash-001");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->response, entry.response);
    EXPECT_EQ(loaded->input_tokens, entry.input_tokens);
    EXPECT_EQ(loaded->output_tokens, entry.output_tokens);

    const auto cache_entries = llm_store.ListPromptCacheEntries("tenant-a", "agent-gateway", "gpt-5.4");
    ASSERT_EQ(cache_entries.size(), 1u);
    EXPECT_EQ(cache_entries.front().prompt_hash, "hash-001");

    const auto key_prefix = mokv::llm::Keyspace::PromptCachePrefix("tenant-a", "agent-gateway", "gpt-5.4");
    const auto raw_entries = store.ListEntriesByPrefix(key_prefix);
    ASSERT_EQ(raw_entries.size(), 1u);
    EXPECT_EQ(raw_entries.front().first,
              mokv::llm::Keyspace::PromptCache("tenant-a", "agent-gateway", "gpt-5.4", "hash-001"));
}

TEST_F(LLMStoreTest, ConversationAppendTrimAndList) {
    mokv::DBKVStore store(TestDBConfig());
    mokv::llm::LLMStore llm_store(store);

    mokv::llm::ConversationDescriptor descriptor;
    descriptor.tenant = "tenant-a";
    descriptor.app_id = "copilot";
    descriptor.session_id = "session-01";
    descriptor.model = "gpt-5.4-mini";

    ASSERT_TRUE(llm_store.AppendConversationTurn(descriptor, {"system", "You are a strict reviewer.", 1}).success);
    ASSERT_TRUE(llm_store.AppendConversationTurn(descriptor, {"user", "Review this diff", 2}).success);
    ASSERT_TRUE(llm_store.AppendConversationTurn(descriptor, {"assistant", "Need more context.", 3}).success);

    const auto sessions = llm_store.ListConversationSessions("tenant-a", "copilot");
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.front().session_id, "session-01");
    EXPECT_EQ(sessions.front().turn_count, 3u);

    const auto turns = llm_store.ListConversationTurns("tenant-a", "copilot", "session-01");
    ASSERT_EQ(turns.size(), 3u);
    EXPECT_EQ(turns[1].role, "user");
    EXPECT_EQ(turns[1].content, "Review this diff");

    auto loaded = llm_store.GetConversation("tenant-a", "copilot", "session-01");
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->turns.size(), 3u);
    EXPECT_EQ(loaded->model, "gpt-5.4-mini");

    ASSERT_TRUE(llm_store.TrimConversationToLastTurns("tenant-a", "copilot", "session-01", 2).success);
    loaded = llm_store.GetConversation("tenant-a", "copilot", "session-01");
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->turns.size(), 2u);
    EXPECT_EQ(loaded->turns.front().role, "user");
    EXPECT_EQ(loaded->turns.back().role, "assistant");
}

TEST_F(LLMStoreTest, RetrievalAndRuntimeConfigRoundTrip) {
    mokv::DBKVStore store(TestDBConfig());
    mokv::llm::LLMStore llm_store(store);

    mokv::llm::RetrievalChunk chunk_a;
    chunk_a.tenant = "tenant-b";
    chunk_a.knowledge_base = "support-center";
    chunk_a.document_id = "doc-42";
    chunk_a.chunk_id = "chunk-7";
    chunk_a.content = "mokv keeps metadata in the KV layer and vectors in the ANN layer.";
    chunk_a.embedding_ref = "vec://support-center/doc-42/chunk-7";
    chunk_a.metadata = {{"lang", "zh-CN"}, {"source", "faq"}};

    mokv::llm::RetrievalChunk chunk_b = chunk_a;
    chunk_b.chunk_id = "chunk-8";
    chunk_b.embedding_ref = "vec://support-center/doc-42/chunk-8";

    ASSERT_TRUE(llm_store.PutRetrievalChunk(chunk_a).success);
    ASSERT_TRUE(llm_store.PutRetrievalChunk(chunk_b).success);

    const auto loaded_chunk = llm_store.GetRetrievalChunk("tenant-b", "support-center", "doc-42", "chunk-7");
    ASSERT_TRUE(loaded_chunk.has_value());
    EXPECT_EQ(loaded_chunk->embedding_ref, chunk_a.embedding_ref);
    ASSERT_EQ(loaded_chunk->metadata.size(), 2u);

    const auto chunks = llm_store.ListRetrievalChunks("tenant-b", "support-center", "doc-42");
    ASSERT_EQ(chunks.size(), 2u);

    mokv::llm::RuntimeConfigRecord production;
    production.tenant = "tenant-b";
    production.app_id = "router";
    production.profile = "production";
    production.payload = "{\"primary_model\":\"gpt-5.4\",\"fallback_model\":\"gpt-5.4-mini\"}";
    production.updated_at_ms = 200;

    mokv::llm::RuntimeConfigRecord canary = production;
    canary.profile = "canary";
    canary.updated_at_ms = 300;

    ASSERT_TRUE(llm_store.PutRuntimeConfig(production).success);
    ASSERT_TRUE(llm_store.PutRuntimeConfig(canary).success);

    const auto loaded_config = llm_store.GetRuntimeConfig("tenant-b", "router", "production");
    ASSERT_TRUE(loaded_config.has_value());
    EXPECT_EQ(loaded_config->payload, production.payload);

    const auto configs = llm_store.ListRuntimeConfigs("tenant-b", "router");
    ASSERT_EQ(configs.size(), 2u);
    EXPECT_EQ(configs.front().profile, "canary");
}
