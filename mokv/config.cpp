#include "mokv/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

namespace mokv {
namespace {

std::string Trim(std::string value) {
    const auto is_space = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

bool ParseBool(std::string value, bool& out) {
    value = Trim(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        out = false;
        return true;
    }
    return false;
}

bool ParseInt32(const std::string& raw, int32_t& out) {
    try {
        const long long value = std::stoll(Trim(raw));
        if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max()) {
            return false;
        }
        out = static_cast<int32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseUInt16(const std::string& raw, uint16_t& out) {
    try {
        const unsigned long value = std::stoul(Trim(raw));
        if (value == 0 || value > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
        out = static_cast<uint16_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseSize(const std::string& raw, size_t& out) {
    try {
        out = static_cast<size_t>(std::stoull(Trim(raw)));
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, delimiter)) {
        parts.push_back(Trim(item));
    }
    return parts;
}

bool ParsePeer(const std::string& raw, MokvConfig::RaftPeer& peer) {
    const auto parts = Split(raw, ',');
    if (parts.size() != 3) {
        return false;
    }

    int32_t id = 0;
    uint16_t port = 0;
    if (!ParseInt32(parts[0], id) || !ParseUInt16(parts[2], port) || parts[1].empty()) {
        return false;
    }

    peer.id = id;
    peer.host = parts[1];
    peer.port = port;
    return true;
}

bool LoadLegacyConfig(std::istream& input, MokvConfig& config) {
    size_t peer_count = 0;
    if (!(input >> peer_count)) {
        return false;
    }

    config = DefaultConfig();
    config.peers.clear();
    for (size_t i = 0; i < peer_count; ++i) {
        MokvConfig::RaftPeer peer;
        int32_t id = 0;
        int32_t port = 0;
        if (!(input >> id >> peer.host >> port)) {
            return false;
        }
        if (port <= 0 || port > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
        peer.id = id;
        peer.port = static_cast<uint16_t>(port);
        config.peers.push_back(peer);
    }

    int32_t local_id = 0;
    int32_t local_port = 0;
    if (!(input >> local_id >> config.host >> local_port)) {
        return false;
    }
    if (local_port <= 0 || local_port > std::numeric_limits<uint16_t>::max()) {
        return false;
    }

    config.node_id = local_id;
    config.port = static_cast<uint16_t>(local_port);

    if (const auto* local_peer = config.FindPeer(config.node_id)) {
        config.host = local_peer->host;
        config.port = local_peer->port;
    } else {
        config.peers.push_back({config.node_id, config.host, config.port});
    }

    return config.Validate();
}

void SyncLocalPeer(MokvConfig& config, bool host_explicit, bool port_explicit) {
    auto it = std::find_if(config.peers.begin(), config.peers.end(), [&](const auto& peer) {
        return peer.id == config.node_id;
    });

    if (it == config.peers.end()) {
        config.peers.push_back({config.node_id, config.host, config.port});
        return;
    }

    if (!host_explicit) {
        config.host = it->host;
    } else {
        it->host = config.host;
    }

    if (!port_explicit) {
        config.port = it->port;
    } else {
        it->port = config.port;
    }
}

const char* GetEnv(const char* name) {
    return std::getenv(name);
}

}  // namespace

bool MokvConfig::LoadFromFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return false;
    }

    std::stringstream buffer;
    buffer << input.rdbuf();
    const std::string content = buffer.str();

    std::stringstream legacy_probe(content);
    std::string first_token;
    legacy_probe >> first_token;
    if (!first_token.empty() && first_token.find('=') == std::string::npos &&
        std::all_of(first_token.begin(), first_token.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        std::stringstream legacy_input(content);
        return LoadLegacyConfig(legacy_input, *this);
    }

    MokvConfig loaded = DefaultConfig();
    loaded.peers.clear();

    bool host_explicit = false;
    bool port_explicit = false;

    std::stringstream modern_input(content);
    std::string line;
    while (std::getline(modern_input, line)) {
        line = Trim(std::move(line));
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            return false;
        }

        const std::string key = Trim(line.substr(0, separator));
        const std::string value = Trim(line.substr(separator + 1));

        if (key == "host") {
            loaded.host = value;
            host_explicit = true;
        } else if (key == "port") {
            if (!ParseUInt16(value, loaded.port)) {
                return false;
            }
            port_explicit = true;
        } else if (key == "data_dir") {
            loaded.data_dir = value;
        } else if (key == "max_memory_mb") {
            if (!ParseSize(value, loaded.max_memory_mb)) {
                return false;
            }
        } else if (key == "block_cache_size_mb") {
            if (!ParseSize(value, loaded.block_cache_size_mb)) {
                return false;
            }
        } else if (key == "memtable_size_mb") {
            if (!ParseSize(value, loaded.memtable_size_mb)) {
                return false;
            }
        } else if (key == "node_id") {
            if (!ParseInt32(value, loaded.node_id)) {
                return false;
            }
        } else if (key == "election_timeout_ms") {
            if (!ParseInt32(value, loaded.election_timeout_ms)) {
                return false;
            }
        } else if (key == "heartbeat_interval_ms") {
            if (!ParseInt32(value, loaded.heartbeat_interval_ms)) {
                return false;
            }
        } else if (key == "snapshot_interval_s") {
            if (!ParseInt32(value, loaded.snapshot_interval_s)) {
                return false;
            }
        } else if (key == "enable_compaction") {
            if (!ParseBool(value, loaded.enable_compaction)) {
                return false;
            }
        } else if (key == "level0_compaction_threshold") {
            if (!ParseSize(value, loaded.level0_compaction_threshold)) {
                return false;
            }
        } else if (key == "background_threads") {
            size_t parsed = 0;
            if (!ParseSize(value, parsed)) {
                return false;
            }
            loaded.background_threads = static_cast<uint32_t>(parsed);
        } else if (key == "max_background_jobs") {
            size_t parsed = 0;
            if (!ParseSize(value, parsed)) {
                return false;
            }
            loaded.max_background_jobs = static_cast<uint32_t>(parsed);
        } else if (key == "verbose_logging") {
            if (!ParseBool(value, loaded.verbose_logging)) {
                return false;
            }
        } else if (key == "log_level") {
            loaded.log_level = value;
        } else if (key == "peer") {
            RaftPeer peer;
            if (!ParsePeer(value, peer)) {
                return false;
            }
            loaded.peers.push_back(peer);
        } else {
            return false;
        }
    }

    if (loaded.peers.empty()) {
        loaded.peers.push_back({loaded.node_id, loaded.host, loaded.port});
    } else {
        SyncLocalPeer(loaded, host_explicit, port_explicit);
    }

    if (!loaded.Validate()) {
        return false;
    }

    *this = std::move(loaded);
    return true;
}

bool MokvConfig::SaveToFile(const std::filesystem::path& path) const {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    std::ofstream output(path);
    if (!output.is_open()) {
        return false;
    }

    output << "host=" << host << '\n'
           << "port=" << port << '\n'
           << "data_dir=" << data_dir.string() << '\n'
           << "max_memory_mb=" << max_memory_mb << '\n'
           << "block_cache_size_mb=" << block_cache_size_mb << '\n'
           << "memtable_size_mb=" << memtable_size_mb << '\n'
           << "node_id=" << node_id << '\n'
           << "election_timeout_ms=" << election_timeout_ms << '\n'
           << "heartbeat_interval_ms=" << heartbeat_interval_ms << '\n'
           << "snapshot_interval_s=" << snapshot_interval_s << '\n'
           << "enable_compaction=" << (enable_compaction ? "true" : "false") << '\n'
           << "level0_compaction_threshold=" << level0_compaction_threshold << '\n'
           << "background_threads=" << background_threads << '\n'
           << "max_background_jobs=" << max_background_jobs << '\n'
           << "verbose_logging=" << (verbose_logging ? "true" : "false") << '\n'
           << "log_level=" << log_level << '\n';

    for (const auto& peer : peers) {
        output << "peer=" << peer.id << ',' << peer.host << ',' << peer.port << '\n';
    }

    return output.good();
}

void MokvConfig::LoadFromEnv() {
    bool host_explicit = false;
    bool port_explicit = false;

    if (const char* value = GetEnv("MOKV_HOST")) {
        host = value;
        host_explicit = true;
    }
    if (const char* value = GetEnv("MOKV_PORT")) {
        port_explicit = ParseUInt16(value, port);
    }
    if (const char* value = GetEnv("MOKV_DATA_DIR")) {
        data_dir = value;
    }
    if (const char* value = GetEnv("MOKV_MAX_MEMORY")) {
        ParseSize(value, max_memory_mb);
    }
    if (const char* value = GetEnv("MOKV_BLOCK_CACHE_MB")) {
        ParseSize(value, block_cache_size_mb);
    }
    if (const char* value = GetEnv("MOKV_MEMTABLE_MB")) {
        ParseSize(value, memtable_size_mb);
    }
    if (const char* value = GetEnv("MOKV_NODE_ID")) {
        ParseInt32(value, node_id);
    }
    if (const char* value = GetEnv("MOKV_ELECTION_TIMEOUT_MS")) {
        ParseInt32(value, election_timeout_ms);
    }
    if (const char* value = GetEnv("MOKV_HEARTBEAT_INTERVAL_MS")) {
        ParseInt32(value, heartbeat_interval_ms);
    }
    if (const char* value = GetEnv("MOKV_SNAPSHOT_INTERVAL_S")) {
        ParseInt32(value, snapshot_interval_s);
    }
    if (const char* value = GetEnv("MOKV_ENABLE_COMPACTION")) {
        ParseBool(value, enable_compaction);
    }
    if (const char* value = GetEnv("MOKV_VERBOSE_LOGGING")) {
        ParseBool(value, verbose_logging);
    }
    if (const char* value = GetEnv("MOKV_LOG_LEVEL")) {
        log_level = value;
    }
    if (const char* value = GetEnv("MOKV_PEERS")) {
        peers.clear();
        for (const auto& item : Split(value, ';')) {
            if (item.empty()) {
                continue;
            }
            RaftPeer peer;
            if (ParsePeer(item, peer)) {
                peers.push_back(peer);
            }
        }
    }

    if (peers.empty()) {
        peers.push_back({node_id, host, port});
    } else {
        SyncLocalPeer(*this, host_explicit, port_explicit);
    }
}

const MokvConfig::RaftPeer* MokvConfig::FindPeer(int32_t id) const {
    const auto it = std::find_if(peers.begin(), peers.end(), [&](const auto& peer) {
        return peer.id == id;
    });
    return it == peers.end() ? nullptr : &(*it);
}

}  // namespace mokv
