#include "mokv/kvstore.hpp"

#include <algorithm>
#include <future>
#include <sstream>

#include "mokv/utils/codec.hpp"

namespace mokv {
namespace {

constexpr std::string_view kMetaKeyIndex = "__mokv__:meta:key_index";

std::string SerializeKeys(const std::vector<std::string>& keys) {
    std::vector<std::string> sorted_keys = keys;
    std::sort(sorted_keys.begin(), sorted_keys.end());

    std::string serialized;
    for (const auto& key : sorted_keys) {
        serialized += codec::Escape(key);
        serialized += '\n';
    }
    return serialized;
}

std::vector<std::string> DeserializeKeys(std::string_view payload) {
    std::vector<std::string> keys;
    std::stringstream stream{std::string(payload)};
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        keys.emplace_back(codec::Unescape(line));
    }
    return keys;
}

DBConfig BuildDBConfig(const MokvConfig& config) {
    DBConfig db_config;
    db_config.data_dir = config.data_dir;
    db_config.memtable_max_size = config.GetMemTableSizeBytes();
    db_config.enable_block_cache = config.block_cache_size_mb > 0;
    db_config.block_cache.max_capacity = config.GetBlockCacheSizeBytes();
    return db_config;
}

}  // namespace

DBKVStore::DBKVStore(const MokvConfig& config)
    : DBKVStore(BuildDBConfig(config)) {}

DBKVStore::DBKVStore(DBConfig config)
    : db_(std::make_shared<DB>(config)) {
    LoadKeyIndex();
}

std::optional<std::string> DBKVStore::Get(std::string_view key) {
    if (!HealthCheck()) {
        return std::nullopt;
    }

    std::string value;
    if (!db_->Get(key, value)) {
        return std::nullopt;
    }
    return value;
}

KVResult DBKVStore::Put(std::string_view key, std::string_view value) {
    if (!HealthCheck()) {
        return KVResult::Fail("DBKVStore is closed");
    }
    if (IsReservedKey(key)) {
        return KVResult::Fail("reserved key prefix is managed by mokv");
    }

    std::optional<std::string> previous = Get(key);
    db_->Put(key, value);

    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        if (std::find(known_keys_.begin(), known_keys_.end(), key) == known_keys_.end()) {
            known_keys_.emplace_back(key);
        }

        if (previous) {
            tracked_bytes_size_ -= key.size() + previous->size();
        }
        tracked_bytes_size_ += key.size() + value.size();
        db_->Put(kMetaKeyIndex, SerializeKeys(known_keys_));
    }

    return KVResult::OK();
}

KVResult DBKVStore::Delete(std::string_view key) {
    if (!HealthCheck()) {
        return KVResult::Fail("DBKVStore is closed");
    }
    if (IsReservedKey(key)) {
        return KVResult::Fail("reserved key prefix is managed by mokv");
    }

    std::optional<std::string> previous = Get(key);
    if (!previous.has_value()) {
        return KVResult::OK();
    }

    db_->Delete(key);

    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        auto it = std::find(known_keys_.begin(), known_keys_.end(), key);
        if (it != known_keys_.end()) {
            known_keys_.erase(it);
        }
        tracked_bytes_size_ -= key.size() + previous->size();
        db_->Put(kMetaKeyIndex, SerializeKeys(known_keys_));
    }

    return KVResult::OK();
}

bool DBKVStore::Exists(std::string_view key) {
    return Get(key).has_value();
}

std::vector<std::string> DBKVStore::ListKeys() {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    return known_keys_;
}

std::vector<std::string> DBKVStore::ListKeysByPrefix(std::string_view prefix) {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    std::vector<std::string> keys;
    keys.reserve(known_keys_.size());
    for (const auto& key : known_keys_) {
        if (prefix.empty() || key.compare(0, prefix.size(), prefix) == 0) {
            keys.emplace_back(key);
        }
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::vector<std::pair<std::string, std::string>> DBKVStore::ListEntriesByPrefix(std::string_view prefix) {
    std::vector<std::pair<std::string, std::string>> entries;
    if (!HealthCheck()) {
        return entries;
    }

    const auto keys = ListKeysByPrefix(prefix);
    entries.reserve(keys.size());
    for (const auto& key : keys) {
        auto value = Get(key);
        if (value) {
            entries.emplace_back(key, *value);
        }
    }
    return entries;
}

KVResult DBKVStore::Clear() {
    std::vector<std::string> keys = ListKeys();
    for (const auto& key : keys) {
        const KVResult result = Delete(key);
        if (!result.success) {
            return result;
        }
    }
    return KVResult::OK();
}

KVFuture DBKVStore::GetAsync(std::string_view key) {
    return std::async(std::launch::async, [this, key = std::string(key)] {
        const auto value = Get(key);
        return value ? KVResult::OK(*value) : KVResult::Fail("key not found");
    });
}

KVFuture DBKVStore::PutAsync(std::string_view key, std::string_view value) {
    return std::async(std::launch::async, [this, key = std::string(key), value = std::string(value)] {
        return Put(key, value);
    });
}

KVFuture DBKVStore::DeleteAsync(std::string_view key) {
    return std::async(std::launch::async, [this, key = std::string(key)] {
        return Delete(key);
    });
}

KVResult DBKVStore::Batch(const std::vector<std::pair<std::string, std::string>>& entries) {
    for (const auto& [key, value] : entries) {
        const KVResult result = Put(key, value);
        if (!result.success) {
            return result;
        }
    }
    return KVResult::OK();
}

KVResult DBKVStore::BatchDelete(const std::vector<std::string>& keys) {
    for (const auto& key : keys) {
        const KVResult result = Delete(key);
        if (!result.success) {
            return result;
        }
    }
    return KVResult::OK();
}

size_t DBKVStore::Size() const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    return known_keys_.size();
}

size_t DBKVStore::BytesSize() const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    return tracked_bytes_size_;
}

bool DBKVStore::Empty() const {
    return Size() == 0;
}

bool DBKVStore::HealthCheck() {
    return !closed_ && db_ != nullptr;
}

void DBKVStore::Close() {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    closed_ = true;
    db_ = nullptr;
    known_keys_.clear();
    tracked_bytes_size_ = 0;
}

DB& DBKVStore::db() {
    return *db_;
}

const DB& DBKVStore::db() const {
    return *db_;
}

void DBKVStore::LoadKeyIndex() {
    if (!db_) {
        return;
    }

    std::string payload;
    if (db_->Get(kMetaKeyIndex, payload)) {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        known_keys_ = DeserializeKeys(payload);
        tracked_bytes_size_ = 0;
        for (const auto& key : known_keys_) {
            std::string value;
            if (db_->Get(key, value)) {
                tracked_bytes_size_ += key.size() + value.size();
            }
        }
    }
}

bool DBKVStore::IsReservedKey(std::string_view key) const {
    constexpr std::string_view kReservedPrefix = "__mokv__:";
    return key.substr(0, std::min(key.size(), kReservedPrefix.size())) == kReservedPrefix;
}

std::unique_ptr<KVStore> CreateKVStore(const MokvConfig& config) {
    return std::make_unique<DBKVStore>(config);
}

}  // namespace mokv
