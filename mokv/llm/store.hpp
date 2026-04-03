#pragma once

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mokv/kvstore.hpp"
#include "mokv/utils/codec.hpp"

namespace mokv {
namespace llm {

struct PromptCacheEntry {
    std::string tenant;
    std::string app_id;
    std::string model;
    std::string prompt_hash;
    std::string response;
    int64_t cached_at_ms = 0;
    uint32_t input_tokens = 0;
    uint32_t output_tokens = 0;
};

struct ConversationTurn {
    std::string role;
    std::string content;
    int64_t created_at_ms = 0;
};

struct ConversationSession {
    std::string tenant;
    std::string app_id;
    std::string session_id;
    std::string model;
    std::vector<ConversationTurn> turns;
};

struct ConversationDescriptor {
    std::string tenant;
    std::string app_id;
    std::string session_id;
    std::string model;
};

struct ConversationSummary {
    std::string tenant;
    std::string app_id;
    std::string session_id;
    std::string model;
    uint32_t turn_count = 0;
    int64_t updated_at_ms = 0;
};

struct RetrievalChunk {
    std::string tenant;
    std::string knowledge_base;
    std::string document_id;
    std::string chunk_id;
    std::string content;
    std::string embedding_ref;
    std::vector<std::pair<std::string, std::string>> metadata;
};

struct RuntimeConfigRecord {
    std::string tenant;
    std::string app_id;
    std::string profile;
    std::string payload;
    int64_t updated_at_ms = 0;
};

namespace detail {

inline bool EndsWith(std::string_view value, std::string_view suffix) {
    if (suffix.size() > value.size()) {
        return false;
    }
    return value.substr(value.size() - suffix.size()) == suffix;
}

inline std::string FormatSequence(uint32_t sequence) {
    std::ostringstream out;
    out << std::setw(10) << std::setfill('0') << sequence;
    return out.str();
}

inline int64_t LastTurnTimestamp(const std::vector<ConversationTurn>& turns) {
    return turns.empty() ? 0 : turns.back().created_at_ms;
}

inline bool AssignInt64(std::string_view raw, int64_t& out) {
    const auto parsed = codec::ParseInt64(raw);
    if (!parsed) {
        return false;
    }
    out = *parsed;
    return true;
}

inline bool AssignUInt32(std::string_view raw, uint32_t& out) {
    const auto parsed = codec::ParseUInt32(raw);
    if (!parsed) {
        return false;
    }
    out = *parsed;
    return true;
}

inline std::string SerializePromptCache(const PromptCacheEntry& entry) {
    std::ostringstream out;
    out << "tenant\t" << codec::Escape(entry.tenant) << '\n'
        << "app_id\t" << codec::Escape(entry.app_id) << '\n'
        << "model\t" << codec::Escape(entry.model) << '\n'
        << "prompt_hash\t" << codec::Escape(entry.prompt_hash) << '\n'
        << "response\t" << codec::Escape(entry.response) << '\n'
        << "cached_at_ms\t" << entry.cached_at_ms << '\n'
        << "input_tokens\t" << entry.input_tokens << '\n'
        << "output_tokens\t" << entry.output_tokens << '\n';
    return out.str();
}

inline std::optional<PromptCacheEntry> ParsePromptCache(std::string_view payload) {
    PromptCacheEntry entry;
    std::stringstream stream{std::string(payload)};
    std::string line;
    while (std::getline(stream, line)) {
        const auto parts = codec::Split(line, '\t');
        if (parts.size() != 2) {
            continue;
        }
        if (parts[0] == "tenant") {
            entry.tenant = codec::Unescape(parts[1]);
        } else if (parts[0] == "app_id") {
            entry.app_id = codec::Unescape(parts[1]);
        } else if (parts[0] == "model") {
            entry.model = codec::Unescape(parts[1]);
        } else if (parts[0] == "prompt_hash") {
            entry.prompt_hash = codec::Unescape(parts[1]);
        } else if (parts[0] == "response") {
            entry.response = codec::Unescape(parts[1]);
        } else if (parts[0] == "cached_at_ms" && !AssignInt64(parts[1], entry.cached_at_ms)) {
            return std::nullopt;
        } else if (parts[0] == "input_tokens" && !AssignUInt32(parts[1], entry.input_tokens)) {
            return std::nullopt;
        } else if (parts[0] == "output_tokens" && !AssignUInt32(parts[1], entry.output_tokens)) {
            return std::nullopt;
        }
    }
    if (entry.model.empty() || entry.prompt_hash.empty()) {
        return std::nullopt;
    }
    return entry;
}

inline std::string SerializeConversationTurn(const ConversationTurn& turn) {
    std::ostringstream out;
    out << "role\t" << codec::Escape(turn.role) << '\n'
        << "content\t" << codec::Escape(turn.content) << '\n'
        << "created_at_ms\t" << turn.created_at_ms << '\n';
    return out.str();
}

inline std::optional<ConversationTurn> ParseConversationTurn(std::string_view payload) {
    ConversationTurn turn;
    std::stringstream stream{std::string(payload)};
    std::string line;
    while (std::getline(stream, line)) {
        const auto parts = codec::Split(line, '\t');
        if (parts.size() != 2) {
            continue;
        }
        if (parts[0] == "role") {
            turn.role = codec::Unescape(parts[1]);
        } else if (parts[0] == "content") {
            turn.content = codec::Unescape(parts[1]);
        } else if (parts[0] == "created_at_ms" && !AssignInt64(parts[1], turn.created_at_ms)) {
            return std::nullopt;
        }
    }
    if (turn.role.empty()) {
        return std::nullopt;
    }
    return turn;
}

inline std::string SerializeConversationSummary(const ConversationSummary& summary) {
    std::ostringstream out;
    out << "tenant\t" << codec::Escape(summary.tenant) << '\n'
        << "app_id\t" << codec::Escape(summary.app_id) << '\n'
        << "session_id\t" << codec::Escape(summary.session_id) << '\n'
        << "model\t" << codec::Escape(summary.model) << '\n'
        << "turn_count\t" << summary.turn_count << '\n'
        << "updated_at_ms\t" << summary.updated_at_ms << '\n';
    return out.str();
}

inline std::optional<ConversationSummary> ParseConversationSummary(std::string_view payload) {
    ConversationSummary summary;
    std::stringstream stream{std::string(payload)};
    std::string line;
    while (std::getline(stream, line)) {
        const auto parts = codec::Split(line, '\t');
        if (parts.size() != 2) {
            continue;
        }
        if (parts[0] == "tenant") {
            summary.tenant = codec::Unescape(parts[1]);
        } else if (parts[0] == "app_id") {
            summary.app_id = codec::Unescape(parts[1]);
        } else if (parts[0] == "session_id") {
            summary.session_id = codec::Unescape(parts[1]);
        } else if (parts[0] == "model") {
            summary.model = codec::Unescape(parts[1]);
        } else if (parts[0] == "turn_count" && !AssignUInt32(parts[1], summary.turn_count)) {
            return std::nullopt;
        } else if (parts[0] == "updated_at_ms" && !AssignInt64(parts[1], summary.updated_at_ms)) {
            return std::nullopt;
        }
    }
    if (summary.session_id.empty()) {
        return std::nullopt;
    }
    return summary;
}

inline std::string SerializeLegacyConversation(const ConversationSession& session) {
    std::ostringstream out;
    out << "tenant\t" << codec::Escape(session.tenant) << '\n'
        << "app_id\t" << codec::Escape(session.app_id) << '\n'
        << "session_id\t" << codec::Escape(session.session_id) << '\n'
        << "model\t" << codec::Escape(session.model) << '\n';
    for (const auto& turn : session.turns) {
        out << "turn\t" << codec::Escape(turn.role) << '\t'
            << turn.created_at_ms << '\t'
            << codec::Escape(turn.content) << '\n';
    }
    return out.str();
}

inline std::optional<ConversationSession> ParseLegacyConversation(std::string_view payload) {
    ConversationSession session;
    std::stringstream stream{std::string(payload)};
    std::string line;
    while (std::getline(stream, line)) {
        const auto parts = codec::Split(line, '\t');
        if (parts.empty()) {
            continue;
        }
        if (parts[0] == "tenant" && parts.size() == 2) {
            session.tenant = codec::Unescape(parts[1]);
        } else if (parts[0] == "app_id" && parts.size() == 2) {
            session.app_id = codec::Unescape(parts[1]);
        } else if (parts[0] == "session_id" && parts.size() == 2) {
            session.session_id = codec::Unescape(parts[1]);
        } else if (parts[0] == "model" && parts.size() == 2) {
            session.model = codec::Unescape(parts[1]);
        } else if (parts[0] == "turn" && parts.size() == 4) {
            int64_t created_at_ms = 0;
            if (!AssignInt64(parts[2], created_at_ms)) {
                return std::nullopt;
            }
            session.turns.push_back(ConversationTurn{
                codec::Unescape(parts[1]),
                codec::Unescape(parts[3]),
                created_at_ms,
            });
        }
    }
    if (session.session_id.empty()) {
        return std::nullopt;
    }
    return session;
}

inline std::string SerializeRetrievalChunk(const RetrievalChunk& chunk) {
    std::ostringstream out;
    out << "tenant\t" << codec::Escape(chunk.tenant) << '\n'
        << "knowledge_base\t" << codec::Escape(chunk.knowledge_base) << '\n'
        << "document_id\t" << codec::Escape(chunk.document_id) << '\n'
        << "chunk_id\t" << codec::Escape(chunk.chunk_id) << '\n'
        << "content\t" << codec::Escape(chunk.content) << '\n'
        << "embedding_ref\t" << codec::Escape(chunk.embedding_ref) << '\n';
    for (const auto& [key, value] : chunk.metadata) {
        out << "meta\t" << codec::Escape(key) << '\t' << codec::Escape(value) << '\n';
    }
    return out.str();
}

inline std::optional<RetrievalChunk> ParseRetrievalChunk(std::string_view payload) {
    RetrievalChunk chunk;
    std::stringstream stream{std::string(payload)};
    std::string line;
    while (std::getline(stream, line)) {
        const auto parts = codec::Split(line, '\t');
        if (parts.empty()) {
            continue;
        }
        if (parts[0] == "tenant" && parts.size() == 2) {
            chunk.tenant = codec::Unescape(parts[1]);
        } else if (parts[0] == "knowledge_base" && parts.size() == 2) {
            chunk.knowledge_base = codec::Unescape(parts[1]);
        } else if (parts[0] == "document_id" && parts.size() == 2) {
            chunk.document_id = codec::Unescape(parts[1]);
        } else if (parts[0] == "chunk_id" && parts.size() == 2) {
            chunk.chunk_id = codec::Unescape(parts[1]);
        } else if (parts[0] == "content" && parts.size() == 2) {
            chunk.content = codec::Unescape(parts[1]);
        } else if (parts[0] == "embedding_ref" && parts.size() == 2) {
            chunk.embedding_ref = codec::Unescape(parts[1]);
        } else if (parts[0] == "meta" && parts.size() == 3) {
            chunk.metadata.emplace_back(codec::Unescape(parts[1]), codec::Unescape(parts[2]));
        }
    }
    if (chunk.document_id.empty() || chunk.chunk_id.empty()) {
        return std::nullopt;
    }
    return chunk;
}

inline std::string SerializeRuntimeConfig(const RuntimeConfigRecord& record) {
    std::ostringstream out;
    out << "tenant\t" << codec::Escape(record.tenant) << '\n'
        << "app_id\t" << codec::Escape(record.app_id) << '\n'
        << "profile\t" << codec::Escape(record.profile) << '\n'
        << "updated_at_ms\t" << record.updated_at_ms << '\n'
        << "payload\t" << codec::Escape(record.payload) << '\n';
    return out.str();
}

inline std::optional<RuntimeConfigRecord> ParseRuntimeConfig(std::string_view payload) {
    RuntimeConfigRecord record;
    std::stringstream stream{std::string(payload)};
    std::string line;
    while (std::getline(stream, line)) {
        const auto parts = codec::Split(line, '\t');
        if (parts.size() != 2) {
            continue;
        }
        if (parts[0] == "tenant") {
            record.tenant = codec::Unescape(parts[1]);
        } else if (parts[0] == "app_id") {
            record.app_id = codec::Unescape(parts[1]);
        } else if (parts[0] == "profile") {
            record.profile = codec::Unescape(parts[1]);
        } else if (parts[0] == "updated_at_ms" && !AssignInt64(parts[1], record.updated_at_ms)) {
            return std::nullopt;
        } else if (parts[0] == "payload") {
            record.payload = codec::Unescape(parts[1]);
        }
    }
    if (record.profile.empty()) {
        return std::nullopt;
    }
    return record;
}

}  // namespace detail

class Keyspace final {
public:
    static std::string PromptCachePrefix(std::string_view tenant,
                                         std::string_view app_id,
                                         std::string_view model = {}) {
        std::string key = codec::JoinKeyParts({"llm", "prompt-cache", tenant, app_id});
        if (!model.empty()) {
            AppendKeyPart(key, model);
        }
        return key;
    }

    static std::string PromptCache(std::string_view tenant,
                                   std::string_view app_id,
                                   std::string_view model,
                                   std::string_view prompt_hash) {
        std::string key = PromptCachePrefix(tenant, app_id, model);
        AppendKeyPart(key, prompt_hash);
        return key;
    }

    static std::string ConversationPrefix(std::string_view tenant,
                                          std::string_view app_id) {
        return codec::JoinKeyParts({"llm", "conversation", tenant, app_id});
    }

    static std::string ConversationSessionPrefix(std::string_view tenant,
                                                 std::string_view app_id,
                                                 std::string_view session_id) {
        std::string key = ConversationPrefix(tenant, app_id);
        AppendKeyPart(key, session_id);
        return key;
    }

    static std::string Conversation(std::string_view tenant,
                                    std::string_view app_id,
                                    std::string_view session_id) {
        return ConversationSessionPrefix(tenant, app_id, session_id);
    }

    static std::string ConversationMeta(std::string_view tenant,
                                        std::string_view app_id,
                                        std::string_view session_id) {
        std::string key = ConversationSessionPrefix(tenant, app_id, session_id);
        AppendKeyPart(key, "meta");
        return key;
    }

    static std::string ConversationTurnPrefix(std::string_view tenant,
                                              std::string_view app_id,
                                              std::string_view session_id) {
        std::string key = ConversationSessionPrefix(tenant, app_id, session_id);
        AppendKeyPart(key, "turn");
        return key;
    }

    static std::string ConversationTurn(std::string_view tenant,
                                        std::string_view app_id,
                                        std::string_view session_id,
                                        uint32_t sequence) {
        std::string key = ConversationTurnPrefix(tenant, app_id, session_id);
        AppendKeyPart(key, detail::FormatSequence(sequence));
        return key;
    }

    static std::string RetrievalPrefix(std::string_view tenant,
                                       std::string_view knowledge_base,
                                       std::string_view document_id = {}) {
        std::string key = codec::JoinKeyParts({"llm", "retrieval", tenant, knowledge_base});
        if (!document_id.empty()) {
            AppendKeyPart(key, document_id);
        }
        return key;
    }

    static std::string Retrieval(std::string_view tenant,
                                 std::string_view knowledge_base,
                                 std::string_view document_id,
                                 std::string_view chunk_id) {
        std::string key = RetrievalPrefix(tenant, knowledge_base, document_id);
        AppendKeyPart(key, chunk_id);
        return key;
    }

    static std::string RuntimeConfigPrefix(std::string_view tenant,
                                           std::string_view app_id) {
        return codec::JoinKeyParts({"llm", "runtime-config", tenant, app_id});
    }

    static std::string RuntimeConfig(std::string_view tenant,
                                     std::string_view app_id,
                                     std::string_view profile) {
        std::string key = RuntimeConfigPrefix(tenant, app_id);
        AppendKeyPart(key, profile);
        return key;
    }

private:
    static void AppendKeyPart(std::string& key, std::string_view part) {
        key += ':';
        key += codec::EscapeKeyPart(part);
    }
};

class PromptCacheStore {
public:
    explicit PromptCacheStore(KVStore& store) : store_(store) {}

    KVResult Put(const PromptCacheEntry& entry) {
        if (entry.prompt_hash.empty() || entry.model.empty()) {
            return KVResult::Fail("prompt cache entry requires model and prompt_hash");
        }
        return store_.Put(Keyspace::PromptCache(entry.tenant, entry.app_id, entry.model, entry.prompt_hash),
                          detail::SerializePromptCache(entry));
    }

    std::optional<PromptCacheEntry> Get(std::string_view tenant,
                                        std::string_view app_id,
                                        std::string_view model,
                                        std::string_view prompt_hash) {
        const auto payload = store_.Get(Keyspace::PromptCache(tenant, app_id, model, prompt_hash));
        if (!payload) {
            return std::nullopt;
        }
        return detail::ParsePromptCache(*payload);
    }

    std::vector<PromptCacheEntry> List(std::string_view tenant,
                                       std::string_view app_id,
                                       std::string_view model) {
        std::vector<PromptCacheEntry> entries;
        for (const auto& item : store_.ListEntriesByPrefix(Keyspace::PromptCachePrefix(tenant, app_id, model))) {
            auto parsed = detail::ParsePromptCache(item.second);
            if (parsed) {
                entries.emplace_back(*parsed);
            }
        }
        std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.cached_at_ms > rhs.cached_at_ms;
        });
        return entries;
    }

    KVResult Delete(std::string_view tenant,
                    std::string_view app_id,
                    std::string_view model,
                    std::string_view prompt_hash) {
        return store_.Delete(Keyspace::PromptCache(tenant, app_id, model, prompt_hash));
    }

private:
    KVStore& store_;
};

class ConversationStore {
public:
    explicit ConversationStore(KVStore& store) : store_(store) {}

    KVResult Save(const ConversationSession& session) {
        if (session.session_id.empty()) {
            return KVResult::Fail("conversation requires session_id");
        }

        const auto stale_turn_keys =
            store_.ListKeysByPrefix(Keyspace::ConversationTurnPrefix(session.tenant, session.app_id, session.session_id));
        if (!stale_turn_keys.empty()) {
            const auto delete_result = store_.BatchDelete(stale_turn_keys);
            if (!delete_result.success) {
                return delete_result;
            }
        }

        ConversationSummary summary;
        summary.tenant = session.tenant;
        summary.app_id = session.app_id;
        summary.session_id = session.session_id;
        summary.model = session.model;
        summary.turn_count = static_cast<uint32_t>(session.turns.size());
        summary.updated_at_ms = detail::LastTurnTimestamp(session.turns);

        std::vector<std::pair<std::string, std::string>> entries;
        entries.reserve(session.turns.size() + 2);
        entries.emplace_back(Keyspace::Conversation(session.tenant, session.app_id, session.session_id),
                             detail::SerializeLegacyConversation(session));
        entries.emplace_back(Keyspace::ConversationMeta(session.tenant, session.app_id, session.session_id),
                             detail::SerializeConversationSummary(summary));
        for (uint32_t i = 0; i < session.turns.size(); ++i) {
            entries.emplace_back(Keyspace::ConversationTurn(session.tenant, session.app_id, session.session_id, i),
                                 detail::SerializeConversationTurn(session.turns[i]));
        }
        return store_.Batch(entries);
    }

    std::optional<ConversationSession> Get(std::string_view tenant,
                                           std::string_view app_id,
                                           std::string_view session_id) {
        const auto summary_payload = store_.Get(Keyspace::ConversationMeta(tenant, app_id, session_id));
        if (summary_payload) {
            auto summary = detail::ParseConversationSummary(*summary_payload);
            if (!summary) {
                return std::nullopt;
            }

            ConversationSession session;
            session.tenant = summary->tenant;
            session.app_id = summary->app_id;
            session.session_id = summary->session_id;
            session.model = summary->model;
            session.turns = ListTurns(tenant, app_id, session_id);
            return session;
        }

        const auto legacy_payload = store_.Get(Keyspace::Conversation(tenant, app_id, session_id));
        if (!legacy_payload) {
            return std::nullopt;
        }
        return detail::ParseLegacyConversation(*legacy_payload);
    }

    KVResult AppendTurn(const ConversationDescriptor& descriptor, const ConversationTurn& turn) {
        ConversationSession session;
        if (const auto existing = Get(descriptor.tenant, descriptor.app_id, descriptor.session_id)) {
            session = *existing;
        } else {
            session.tenant = descriptor.tenant;
            session.app_id = descriptor.app_id;
            session.session_id = descriptor.session_id;
            session.model = descriptor.model;
        }

        if (!descriptor.model.empty()) {
            session.model = descriptor.model;
        }
        session.turns.emplace_back(turn);
        return Save(session);
    }

    std::vector<ConversationSummary> List(std::string_view tenant, std::string_view app_id) {
        std::vector<ConversationSummary> sessions;
        const auto prefix = Keyspace::ConversationPrefix(tenant, app_id);
        for (const auto& item : store_.ListEntriesByPrefix(prefix)) {
            if (!detail::EndsWith(item.first, ":meta")) {
                continue;
            }
            auto parsed = detail::ParseConversationSummary(item.second);
            if (parsed) {
                sessions.emplace_back(*parsed);
            }
        }
        std::sort(sessions.begin(), sessions.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.updated_at_ms > rhs.updated_at_ms;
        });
        return sessions;
    }

    std::vector<ConversationTurn> ListTurns(std::string_view tenant,
                                            std::string_view app_id,
                                            std::string_view session_id) {
        std::vector<ConversationTurn> turns;
        for (const auto& item :
             store_.ListEntriesByPrefix(Keyspace::ConversationTurnPrefix(tenant, app_id, session_id))) {
            auto parsed = detail::ParseConversationTurn(item.second);
            if (parsed) {
                turns.emplace_back(*parsed);
            }
        }
        return turns;
    }

    KVResult TrimToLastTurns(std::string_view tenant,
                             std::string_view app_id,
                             std::string_view session_id,
                             size_t keep_last_turns) {
        auto session = Get(tenant, app_id, session_id);
        if (!session) {
            return KVResult::Fail("conversation not found");
        }
        if (session->turns.size() <= keep_last_turns) {
            return KVResult::OK();
        }
        session->turns.erase(session->turns.begin(),
                             session->turns.begin() + (session->turns.size() - keep_last_turns));
        return Save(*session);
    }

    KVResult Delete(std::string_view tenant,
                    std::string_view app_id,
                    std::string_view session_id) {
        std::vector<std::string> keys =
            store_.ListKeysByPrefix(Keyspace::ConversationTurnPrefix(tenant, app_id, session_id));
        keys.emplace_back(Keyspace::ConversationMeta(tenant, app_id, session_id));
        keys.emplace_back(Keyspace::Conversation(tenant, app_id, session_id));
        return store_.BatchDelete(keys);
    }

private:
    KVStore& store_;
};

class RetrievalStore {
public:
    explicit RetrievalStore(KVStore& store) : store_(store) {}

    KVResult Put(const RetrievalChunk& chunk) {
        if (chunk.document_id.empty() || chunk.chunk_id.empty()) {
            return KVResult::Fail("retrieval chunk requires document_id and chunk_id");
        }
        return store_.Put(Keyspace::Retrieval(chunk.tenant, chunk.knowledge_base, chunk.document_id, chunk.chunk_id),
                          detail::SerializeRetrievalChunk(chunk));
    }

    std::optional<RetrievalChunk> Get(std::string_view tenant,
                                      std::string_view knowledge_base,
                                      std::string_view document_id,
                                      std::string_view chunk_id) {
        const auto payload = store_.Get(Keyspace::Retrieval(tenant, knowledge_base, document_id, chunk_id));
        if (!payload) {
            return std::nullopt;
        }
        return detail::ParseRetrievalChunk(*payload);
    }

    std::vector<RetrievalChunk> List(std::string_view tenant,
                                     std::string_view knowledge_base,
                                     std::string_view document_id) {
        std::vector<RetrievalChunk> chunks;
        for (const auto& item : store_.ListEntriesByPrefix(Keyspace::RetrievalPrefix(tenant, knowledge_base, document_id))) {
            auto parsed = detail::ParseRetrievalChunk(item.second);
            if (parsed) {
                chunks.emplace_back(*parsed);
            }
        }
        return chunks;
    }

    KVResult Delete(std::string_view tenant,
                    std::string_view knowledge_base,
                    std::string_view document_id,
                    std::string_view chunk_id) {
        return store_.Delete(Keyspace::Retrieval(tenant, knowledge_base, document_id, chunk_id));
    }

private:
    KVStore& store_;
};

class RuntimeConfigStore {
public:
    explicit RuntimeConfigStore(KVStore& store) : store_(store) {}

    KVResult Put(const RuntimeConfigRecord& record) {
        if (record.profile.empty()) {
            return KVResult::Fail("runtime config requires profile");
        }
        return store_.Put(Keyspace::RuntimeConfig(record.tenant, record.app_id, record.profile),
                          detail::SerializeRuntimeConfig(record));
    }

    std::optional<RuntimeConfigRecord> Get(std::string_view tenant,
                                           std::string_view app_id,
                                           std::string_view profile) {
        const auto payload = store_.Get(Keyspace::RuntimeConfig(tenant, app_id, profile));
        if (!payload) {
            return std::nullopt;
        }
        return detail::ParseRuntimeConfig(*payload);
    }

    std::vector<RuntimeConfigRecord> List(std::string_view tenant,
                                          std::string_view app_id) {
        std::vector<RuntimeConfigRecord> records;
        for (const auto& item : store_.ListEntriesByPrefix(Keyspace::RuntimeConfigPrefix(tenant, app_id))) {
            auto parsed = detail::ParseRuntimeConfig(item.second);
            if (parsed) {
                records.emplace_back(*parsed);
            }
        }
        std::sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.updated_at_ms > rhs.updated_at_ms;
        });
        return records;
    }

    KVResult Delete(std::string_view tenant,
                    std::string_view app_id,
                    std::string_view profile) {
        return store_.Delete(Keyspace::RuntimeConfig(tenant, app_id, profile));
    }

private:
    KVStore& store_;
};

class LLMStore {
public:
    explicit LLMStore(KVStore& store)
        : prompt_cache_(store),
          conversations_(store),
          retrieval_(store),
          runtime_configs_(store) {}

    PromptCacheStore& prompt_cache() {
        return prompt_cache_;
    }

    ConversationStore& conversations() {
        return conversations_;
    }

    RetrievalStore& retrieval() {
        return retrieval_;
    }

    RuntimeConfigStore& runtime_configs() {
        return runtime_configs_;
    }

    KVResult PutPromptCache(const PromptCacheEntry& entry) {
        return prompt_cache_.Put(entry);
    }

    std::optional<PromptCacheEntry> GetPromptCache(std::string_view tenant,
                                                   std::string_view app_id,
                                                   std::string_view model,
                                                   std::string_view prompt_hash) {
        return prompt_cache_.Get(tenant, app_id, model, prompt_hash);
    }

    std::vector<PromptCacheEntry> ListPromptCacheEntries(std::string_view tenant,
                                                         std::string_view app_id,
                                                         std::string_view model) {
        return prompt_cache_.List(tenant, app_id, model);
    }

    KVResult DeletePromptCache(std::string_view tenant,
                               std::string_view app_id,
                               std::string_view model,
                               std::string_view prompt_hash) {
        return prompt_cache_.Delete(tenant, app_id, model, prompt_hash);
    }

    KVResult SaveConversation(const ConversationSession& session) {
        return conversations_.Save(session);
    }

    std::optional<ConversationSession> GetConversation(std::string_view tenant,
                                                       std::string_view app_id,
                                                       std::string_view session_id) {
        return conversations_.Get(tenant, app_id, session_id);
    }

    KVResult AppendConversationTurn(const ConversationDescriptor& descriptor,
                                    const ConversationTurn& turn) {
        return conversations_.AppendTurn(descriptor, turn);
    }

    std::vector<ConversationSummary> ListConversationSessions(std::string_view tenant,
                                                              std::string_view app_id) {
        return conversations_.List(tenant, app_id);
    }

    std::vector<ConversationTurn> ListConversationTurns(std::string_view tenant,
                                                        std::string_view app_id,
                                                        std::string_view session_id) {
        return conversations_.ListTurns(tenant, app_id, session_id);
    }

    KVResult TrimConversationToLastTurns(std::string_view tenant,
                                         std::string_view app_id,
                                         std::string_view session_id,
                                         size_t keep_last_turns) {
        return conversations_.TrimToLastTurns(tenant, app_id, session_id, keep_last_turns);
    }

    KVResult DeleteConversation(std::string_view tenant,
                                std::string_view app_id,
                                std::string_view session_id) {
        return conversations_.Delete(tenant, app_id, session_id);
    }

    KVResult PutRetrievalChunk(const RetrievalChunk& chunk) {
        return retrieval_.Put(chunk);
    }

    std::optional<RetrievalChunk> GetRetrievalChunk(std::string_view tenant,
                                                    std::string_view knowledge_base,
                                                    std::string_view document_id,
                                                    std::string_view chunk_id) {
        return retrieval_.Get(tenant, knowledge_base, document_id, chunk_id);
    }

    std::vector<RetrievalChunk> ListRetrievalChunks(std::string_view tenant,
                                                    std::string_view knowledge_base,
                                                    std::string_view document_id) {
        return retrieval_.List(tenant, knowledge_base, document_id);
    }

    KVResult DeleteRetrievalChunk(std::string_view tenant,
                                  std::string_view knowledge_base,
                                  std::string_view document_id,
                                  std::string_view chunk_id) {
        return retrieval_.Delete(tenant, knowledge_base, document_id, chunk_id);
    }

    KVResult PutRuntimeConfig(const RuntimeConfigRecord& record) {
        return runtime_configs_.Put(record);
    }

    std::optional<RuntimeConfigRecord> GetRuntimeConfig(std::string_view tenant,
                                                        std::string_view app_id,
                                                        std::string_view profile) {
        return runtime_configs_.Get(tenant, app_id, profile);
    }

    std::vector<RuntimeConfigRecord> ListRuntimeConfigs(std::string_view tenant,
                                                        std::string_view app_id) {
        return runtime_configs_.List(tenant, app_id);
    }

    KVResult DeleteRuntimeConfig(std::string_view tenant,
                                 std::string_view app_id,
                                 std::string_view profile) {
        return runtime_configs_.Delete(tenant, app_id, profile);
    }

private:
    PromptCacheStore prompt_cache_;
    ConversationStore conversations_;
    RetrievalStore retrieval_;
    RuntimeConfigStore runtime_configs_;
};

}  // namespace llm
}  // namespace mokv
