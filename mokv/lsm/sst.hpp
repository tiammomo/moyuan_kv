/**
 * @file sst.hpp
 * @brief SST (Sorted String Table) 磁盘存储实现
 *
 * SST 是 LSM 树的持久化组件，负责将内存中的有序数据写入磁盘。
 * 使用 mmap 映射文件以利用 OS 页缓存，减少内存拷贝。
 *
 * @section sst_structure SST 文件格式
 * ┌─────────────────────────────────────────────────────────────────┐
 * │                    SST 文件结构                                  │
 * ├─────────────────────────────────────────────────────────────────┤
 * │                                                                 │
 * │   [IndexBlock] | [DataBlock 0] | [DataBlock 1] | ...            │
 * │                                                                 │
 * │   IndexBlock: [size(8B)] [count(8B)] [(offset + key_size + key)] │
 * │   - 存储每个 DataBlock 的起始偏移量和起始 key                    │
 * │   - 用于快速定位目标 key 所在的 DataBlock                       │
 * │                                                                 │
 * │   DataBlock: [size(8B)] [bloom_filter] [count] [(key + value)]  │
 * │   - 存储实际的键值对数据                                         │
 * │   - 包含布隆过滤器用于快速判断 key 是否存在                      │
 * │                                                                 │
 * @section features 主要特性
 * - mmap 文件映射：利用 OS 页缓存
 * - 布隆过滤器：减少无效磁盘 I/O
 * - 可选压缩：LZ4/Snappy 压缩支持
 * - Block Cache：热点数据加速读取
 *                                                                 │
 * @section read_flow 查询流程
 *   1. 在 IndexBlock 中二分查找目标 key 所在的 DataBlock
 *   2. 加载对应 DataBlock，使用布隆过滤器快速过滤
 *   3. 在 DataBlock 中二分查找目标 key
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/mman.h>

#include "mokv/utils/bloom_filter.hpp"
#include "mokv/utils/compression.hpp"
#include "mokv/lsm/memtable.hpp"
#include "mokv/lsm/block_cache.hpp"

namespace mokv {
namespace lsm {
namespace detail {

inline void EnsureFileSizeOrThrow(int fd, size_t file_size, const std::string& file_name) {
    if (ftruncate(fd, static_cast<off_t>(file_size)) == -1) {
        throw std::runtime_error("failed to resize file: " + file_name);
    }
}

inline size_t ReadSize(const char* ptr) {
    size_t value = 0;
    std::memcpy(&value, ptr, sizeof(value));
    return value;
}

inline void WriteSize(char* ptr, size_t value) {
    std::memcpy(ptr, &value, sizeof(value));
}

inline uint32_t ReadUInt32(const char* ptr) {
    uint32_t value = 0;
    std::memcpy(&value, ptr, sizeof(value));
    return value;
}

inline void WriteUInt32(char* ptr, uint32_t value) {
    std::memcpy(ptr, &value, sizeof(value));
}

constexpr uint32_t kEncodedBlockMagic = 0x564b4f4d;  // "MOKV"
constexpr uint8_t kEncodedBlockVersion = 1;
constexpr uint8_t kEncodedBlockFlagCompressed = 0x01;
constexpr size_t kEncodedBlockHeaderSize =
    sizeof(uint32_t) + 4 * sizeof(uint8_t) + sizeof(size_t);

inline bool IsEncodedBlock(const char* payload, size_t payload_size) {
    if (payload_size < kEncodedBlockHeaderSize) {
        return false;
    }
    return ReadUInt32(payload) == kEncodedBlockMagic;
}

inline void WriteEncodedBlockHeader(char* payload,
                                    bool compressed,
                                    common::CompressionType compression_type,
                                    size_t entries_size) {
    WriteUInt32(payload, kEncodedBlockMagic);
    payload += sizeof(uint32_t);
    *payload++ = static_cast<char>(kEncodedBlockVersion);
    *payload++ = static_cast<char>(compressed ? kEncodedBlockFlagCompressed : 0);
    *payload++ = static_cast<char>(compression_type);
    *payload++ = 0;
    WriteSize(payload, entries_size);
}

}  // namespace detail

/**
 * @brief 压缩配置
 */
struct CompressionConfig {
    common::CompressionType type = common::CompressionType::kLZ4;  ///< 压缩算法类型
    bool enable = true;                             ///< 是否启用压缩
    size_t min_size_for_compression = 64;          ///< 最小压缩大小（小于此值不压缩）

    /// @brief 判断是否应该压缩
    bool ShouldCompress(size_t original_size) const {
        return enable && original_size >= min_size_for_compression;
    }
};

/**
 * EntryIndex - DataBlock 中的条目索引
 * 记录每个键值对的偏移量、键和值
 *
 * 注意: std::string_view 指向的内存必须在 SST 文件生命周期内有效
 * 这些视图在 Load() 时指向 mmap 后的文件内存
 */
struct EntryIndex {
    std::string_view key;       // 键的视图（指向 mmap 内存）
    std::string_view value;     // 值的视图（指向 mmap 内存）
    size_t offset;              // 在 DataBlock 中的偏移量

    size_t binary_size() {
        return key.size() + value.size() + 2 * sizeof(size_t);
    }

    size_t Load(char* s, size_t index) {
        offset = index;
        key = std::string_view(s + 2 * sizeof(size_t), *reinterpret_cast<size_t*>(s));
        value = std::string_view(s + 2 * sizeof(size_t) + key.size(), *reinterpret_cast<size_t*>(s + sizeof(size_t)));
        return binary_size();
    }
};
/*
 * SST 文件格式说明:
 * ┌─────────────────────────────────────────────────────────────────┐
 * │                        SST 文件结构                              │
 * ├─────────────────────────────────────────────────────────────────┤
 * │  [IndexBlock] | [DataBlock 0] | [DataBlock 1] | ...              │
 * └─────────────────────────────────────────────────────────────────┘
 *
 * IndexBlock: [size(8B) | count(8B) | (offset(8B) + key_size(8B) + key), ...]
 *   - 存储每个 DataBlock 的起始偏移量和起始 key
 *   - 用于快速定位目标 key 所在的 DataBlock
 *
 * DataBlock: [size(8B) | bloom_filter | count(8B) | (key_size + value_size + key + value), ...]
 *   - 存储实际的键值对数据
 *   - 包含布隆过滤器用于快速判断 key 是否存在
 *
 * 访问流程:
 *   1. 在 IndexBlock 中二分查找目标 key 所在的 DataBlock
 *   2. 加载对应 DataBlock，使用布隆过滤器快速过滤
 *   3. 在 DataBlock 中二分查找目标 key
 */

class DataBlockIndex {
public:
    void SetOffset(size_t offset) {
        offset_ = offset;
    }
    size_t Load(char* s, int32_t offset = -1) {
        if (offset != -1) {
            offset_ = static_cast<size_t>(offset);
        }
        data_index_.clear();
        entry_storage_.clear();
        compressed_ = false;
        compression_type_ = common::CompressionType::kNone;
        binary_size_cached_ = false;

        const char* block = s + offset_;
        cached_binary_size_ = detail::ReadSize(block);
        const char* payload = block + sizeof(size_t);
        const size_t payload_size =
            cached_binary_size_ > sizeof(size_t) ? cached_binary_size_ - sizeof(size_t) : 0;

        if (detail::IsEncodedBlock(payload, payload_size)) {
            LoadEncodedBlock(payload, payload_size);
            return offset_ + cached_binary_size_;
        }

        size_t index = offset_ + sizeof(size_t);
        index += bloom_filter_.Load(s + index);
        ParseEntries(s + index, cached_binary_size_ - (index - offset_));
        return offset_ + cached_binary_size_;
    }
    size_t binary_size() {
        if (!binary_size_cached_) {
            size_t size = 2 * sizeof(size_t);
            for (auto& entry : data_index_) {
                size += entry.binary_size();
            }
            cached_binary_size_ = size;
            binary_size_cached_ = true;
        }
        return cached_binary_size_;
    }
    bool Get(std::string_view key, std::string& value) {
        if (!bloom_filter_.Check(key.data(), key.size())) {
            return false;
        }
        {
            size_t l = 0, r = data_index_.size();
            while (l < r) {
                size_t mid = (l + r) >> 1;
                if (data_index_[mid].key < key) {
                    l = mid + 1;
                } else {
                    r = mid;
                }
            }
            if (r == data_index_.size() || data_index_[r].key != key) {
                return false;
            } else {
                value = data_index_[r].value; // copy
                return true;
            }
        }
        return false;
    };

    std::vector<EntryIndex>& data_index() {
        return data_index_;
    }

    bool IsCompressed() const {
        return compressed_;
    }

    common::CompressionType compression_type() const {
        return compression_type_;
    }

protected:
    /// @brief 加载布隆过滤器（供子类使用）
    size_t LoadBloomFilter(char* s) {
        return bloom_filter_.Load(s);
    }

private:
    void ParseEntries(const char* entries_data, size_t entries_size) {
        if (entries_size < sizeof(size_t)) {
            size_ = 0;
            return;
        }

        size_ = detail::ReadSize(entries_data);
        size_t index = sizeof(size_t);
        data_index_.reserve(size_);
        for (size_t i = 0; i < size_; ++i) {
            if (index + 2 * sizeof(size_t) > entries_size) {
                data_index_.clear();
                size_ = 0;
                return;
            }

            const size_t key_size = detail::ReadSize(entries_data + index);
            const size_t value_size =
                detail::ReadSize(entries_data + index + sizeof(size_t));
            const size_t record_size = 2 * sizeof(size_t) + key_size + value_size;
            if (index + record_size > entries_size) {
                data_index_.clear();
                size_ = 0;
                return;
            }

            EntryIndex entry_index;
            entry_index.offset = index;
            entry_index.key =
                std::string_view(entries_data + index + 2 * sizeof(size_t), key_size);
            entry_index.value = std::string_view(
                entries_data + index + 2 * sizeof(size_t) + key_size,
                value_size);
            data_index_.emplace_back(std::move(entry_index));
            index += record_size;
        }
    }

    void LoadEncodedBlock(const char* payload, size_t payload_size) {
        const uint8_t version = static_cast<uint8_t>(payload[sizeof(uint32_t)]);
        if (version != detail::kEncodedBlockVersion) {
            size_ = 0;
            return;
        }

        const uint8_t flags =
            static_cast<uint8_t>(payload[sizeof(uint32_t) + sizeof(uint8_t)]);
        compressed_ = (flags & detail::kEncodedBlockFlagCompressed) != 0;
        compression_type_ = static_cast<common::CompressionType>(
            static_cast<uint8_t>(payload[sizeof(uint32_t) + 2 * sizeof(uint8_t)]));

        const size_t entries_size = detail::ReadSize(
            payload + sizeof(uint32_t) + 4 * sizeof(uint8_t));

        size_t index = detail::kEncodedBlockHeaderSize;
        index += bloom_filter_.Load(const_cast<char*>(payload + index));
        if (index > payload_size) {
            data_index_.clear();
            size_ = 0;
            return;
        }

        const char* encoded_entries = payload + index;
        const size_t encoded_entries_size = payload_size - index;
        if (!compressed_) {
            ParseEntries(encoded_entries, encoded_entries_size);
            return;
        }

        auto compressor = common::CompressionFactory::Create(compression_type_);
        if (!compressor) {
            data_index_.clear();
            size_ = 0;
            return;
        }

        common::CompressedData compressed_entries(
            encoded_entries,
            encoded_entries_size,
            entries_size);
        entry_storage_.resize(entries_size);
        const size_t decompressed_size = compressor->Decompress(
            compressed_entries,
            entry_storage_.data(),
            entry_storage_.size());
        if (decompressed_size != entries_size) {
            entry_storage_.clear();
            data_index_.clear();
            size_ = 0;
            return;
        }

        ParseEntries(entry_storage_.data(), entry_storage_.size());
    }

private:
    size_t offset_{0};
    std::vector<EntryIndex> data_index_;
    std::vector<char> entry_storage_;
    mokv::common::BloomFilter bloom_filter_;
    size_t cached_binary_size_{0};
    bool binary_size_cached_{false};
    size_t size_{0};
    bool compressed_{false};
    common::CompressionType compression_type_{common::CompressionType::kNone};
};

/**
 * @brief CompressedDataBlock - 支持压缩的 DataBlock
 *
 * 扩展 DataBlockIndex 功能，支持：
 * - 透明压缩/解压
 * - 懒加载（首次访问时才解压）
 * - 压缩率统计
 */
class CompressedDataBlock : public DataBlockIndex {
public:
    CompressedDataBlock() : compressed_(false), decompressed_(false) {}

    /**
     * @brief 从压缩数据加载
     * @param compressed_data 压缩数据指针
     * @param compressed_size 压缩数据大小
     * @param config 压缩配置
     * @return 加载的字节数
     */
    size_t LoadCompressed(const char* compressed_data, size_t compressed_size,
                          const CompressionConfig& config) {
        CompressionConfig_ = config;

        // 读取压缩头信息
        size_t index = 0;
        compressed_size_ = *reinterpret_cast<const size_t*>(compressed_data);
        index += sizeof(size_t);

        uint8_t compression_flags = static_cast<uint8_t>(compressed_data[index]);
        index += sizeof(uint8_t);

        compressed_ = (compression_flags & 0x01);
        has_compression_header_ = (compression_flags & 0x02);

        // 加载布隆过滤器（始终不压缩）
        index += LoadBloomFilter(const_cast<char*>(compressed_data + index));

        // 加载压缩的条目数据
        compressed_entries_data_.assign(compressed_data + index,
                                         compressed_data + compressed_size);
        decompressed_ = !compressed_;

        return compressed_size;
    }

    /**
     * @brief 解压数据块（懒加载）
     * @return 解压后的数据指针
     */
    const char* EnsureDecompressed() {
        if (!compressed_ || decompressed_) {
            return compressed_entries_data_.data();
        }

        auto compressor = common::CompressionFactory::Create(CompressionConfig_.type);
        if (!compressor) {
            return compressed_entries_data_.data();
        }

        common::CompressedData compressed(compressed_entries_data_.data(),
                                           compressed_entries_data_.size(),
                                           compressed_size_);

        decompressed_data_.resize(compressed_size_);
        size_t result = compressor->Decompress(compressed,
                                                decompressed_data_.data(),
                                                compressed_size_);

        if (result > 0) {
            decompressed_ = true;
            // 释放压缩数据，节省内存
            std::vector<char>().swap(compressed_entries_data_);
            return decompressed_data_.data();
        }

        return compressed_entries_data_.data();
    }

    /**
     * @brief 获取压缩率
     * @return 压缩率（0-1，1表示无压缩）
     */
    double GetCompressionRatio() const {
        if (!compressed_ || decompressed_data_.empty()) {
            return 1.0;
        }
        return static_cast<double>(compressed_entries_data_.size()) /
               decompressed_data_.size();
    }

    /// @brief 是否已压缩
    bool IsCompressed() const { return compressed_; }

    /// @brief 是否已解压
    bool IsDecompressed() const { return decompressed_; }

    /// @brief 获取压缩后大小
    size_t compressed_size() const { return compressed_size_; }

private:
    CompressionConfig CompressionConfig_;           ///< 压缩配置
    size_t compressed_size_{0};                     ///< 压缩后大小
    bool compressed_{false};                        ///< 是否已压缩
    bool has_compression_header_{true};             ///< 是否有压缩头
    bool decompressed_{false};                      ///< 是否已解压
    std::vector<char> compressed_entries_data_;     ///< 压缩的条目数据
    std::vector<char> decompressed_data_;           ///< 解压后的数据
};

/**
 * @brief 压缩数据块构建器
 *
 * 用于构建压缩的 DataBlock，提供：
 * - 键值对累加
 * - 布隆过滤器构建
 * - 数据压缩
 */
class CompressedBlockBuilder {
public:
    CompressedBlockBuilder(const CompressionConfig& config = CompressionConfig())
        : config_(config), total_size_(0), count_(0) {}

    /// @brief 添加键值对
    void Add(std::string_view key, std::string_view value) {
        // 序列化键值对
        size_t entry_size = 2 * sizeof(size_t) + key.size() + value.size();
        raw_data_.reserve(entry_size);

        char size_buf[2 * sizeof(size_t)];
        detail::WriteSize(size_buf, key.size());
        detail::WriteSize(size_buf + sizeof(size_t), value.size());
        raw_data_.insert(raw_data_.end(), size_buf, size_buf + 2 * sizeof(size_t));
        raw_data_.insert(raw_data_.end(), key.data(), key.data() + key.size());
        raw_data_.insert(raw_data_.end(), value.data(), value.data() + value.size());
        keys_.emplace_back(key);

        total_size_ += entry_size;
        ++count_;
    }

    /// @brief 获取键值对数量
    size_t count() const { return count_; }

    /// @brief 获取原始数据大小
    size_t raw_size() const { return total_size_; }

    /// @brief 获取构建时是否实际启用了压缩
    bool used_compression() const { return used_compression_; }

    /// @brief 获取未压缩 DataBlock 大小
    size_t uncompressed_block_size() const { return uncompressed_block_size_; }

    /// @brief 构建并压缩
    /// @return 压缩后的数据
    std::vector<char> Build() {
        std::vector<char> result;

        if (count_ == 0) {
            bloom_filter_.Init(1, 0.01);
        } else {
            bloom_filter_.Init(count_, 0.01);
            for (const auto& key : keys_) {
                bloom_filter_.Insert(key, key.size());
            }
        }

        size_t bloom_size = bloom_filter_.binary_size();
        size_t entries_size = sizeof(size_t) + raw_data_.size();
        size_t payload_size = detail::kEncodedBlockHeaderSize + bloom_size + entries_size;
        uncompressed_block_size_ = sizeof(size_t) + payload_size;

        // 决定是否压缩
        bool do_compress = config_.ShouldCompress(payload_size);
        used_compression_ = false;

        // 构建结果
        // [magic | version | flags | compression_type | reserved | entries_size | bloom_filter | count | data]
        result.resize(payload_size);
        detail::WriteEncodedBlockHeader(result.data(),
                                        false,
                                        config_.type,
                                        entries_size);

        // 写入布隆过滤器
        size_t data_offset = detail::kEncodedBlockHeaderSize;
        bloom_filter_.Save(result.data() + data_offset);

        // 写入 count
        detail::WriteSize(result.data() + data_offset + bloom_size, count_);

        // 写入数据
        data_offset += bloom_size + sizeof(size_t);
        memcpy(result.data() + data_offset, raw_data_.data(), raw_data_.size());

        // 如果需要压缩
        if (do_compress) {
            auto compressor = common::CompressionFactory::Create(config_.type);
            if (compressor) {
                const size_t entries_offset = detail::kEncodedBlockHeaderSize + bloom_size;
                common::CompressedData compressed = compressor->Compress(
                    result.data() + entries_offset,
                    entries_size);

                if (!compressed.empty() && compressed.size() < entries_size) {
                    std::vector<char> compressed_result;
                    compressed_result.resize(detail::kEncodedBlockHeaderSize + bloom_size + compressed.size());
                    detail::WriteEncodedBlockHeader(compressed_result.data(),
                                                    true,
                                                    config_.type,
                                                    entries_size);

                    // Bloom filter 保持未压缩，便于快速过滤
                    bloom_filter_.Save(compressed_result.data() + detail::kEncodedBlockHeaderSize);

                    // 写入压缩后的 count + entries
                    memcpy(compressed_result.data() + entries_offset,
                           compressed.data(), compressed.size());

                    result = std::move(compressed_result);
                    used_compression_ = true;
                }
            }
        }

        return result;
    }

private:
    CompressionConfig config_;
    std::vector<char> raw_data_;
    std::vector<std::string> keys_;
    common::BloomFilter bloom_filter_;
    size_t total_size_;
    size_t count_;
    bool used_compression_{false};
    size_t uncompressed_block_size_{0};
};

/**
 * @brief CachedDataBlock - 支持缓存的 DataBlock
 *
 * 扩展 DataBlockIndex 功能，支持：
 * - BlockCache 集成
 * - 懒加载（首次访问时从缓存加载）
 * - 缓存命中率统计
 */
class CachedDataBlock : public DataBlockIndex {
public:
    CachedDataBlock() : cache_(nullptr), sst_id_(0), block_offset_(0), cached_(false) {}

    /**
     * @brief 初始化缓存引用
     * @param cache 缓存指针
     * @param sst_id SST ID
     * @param block_offset 块偏移量
     */
    void InitCache(BlockCache* cache, size_t sst_id, size_t block_offset) {
        cache_ = cache;
        sst_id_ = sst_id;
        block_offset_ = block_offset;
    }

    /**
     * @brief 尝试从缓存加载
     * @return 缓存的数据指针，未命中返回 nullptr
     */
    const std::vector<char>* LoadFromCache() {
        if (!cache_) return nullptr;

        auto data = cache_->Get(sst_id_, block_offset_);
        if (data) {
            cached_ = true;
            return data;
        }
        return nullptr;
    }

    /**
     * @brief 将数据放入缓存
     * @param data 要缓存的数据
     * @return true 缓存成功
     */
    bool PutToCache(std::vector<char>&& data) {
        if (!cache_ || cached_) return false;
        return cache_->Put(sst_id_, block_offset_, std::move(data));
    }

    /// @brief 是否已缓存
    bool IsCached() const { return cached_; }

    /// @brief 获取 SST ID
    size_t sst_id() const { return sst_id_; }

    /// @brief 获取块偏移量
    size_t block_offset() const { return block_offset_; }

private:
    BlockCache* cache_;    ///< 缓存指针（不拥有所有权）
    size_t sst_id_;        ///< SST ID
    size_t block_offset_;  ///< 块偏移量
    bool cached_;          ///< 是否已缓存
};

class DataBlockIndexIndex {
public:
    DataBlockIndex& Get() {
        return data_block_index_;
    }

    size_t Load(char* s, char* data) {
        offset_ = *reinterpret_cast<size_t*>(s);
        key_ = std::string_view(s + 2 * sizeof(size_t), *reinterpret_cast<size_t*>(s + sizeof(size_t)));
        // std::cout << " key " << key_ << " offset " << offset_ << std::endl;
        data_block_index_.Load(data, offset_);
        return 2 * sizeof(size_t) + key_.size();
    }

    const std::string_view key() const {
        return key_;
    }

    /// @brief 获取块偏移量
    size_t offset() const {
        return offset_;
    }

    bool IsCompressed() const {
        return data_block_index_.IsCompressed();
    }

    common::CompressionType compression_type() const {
        return data_block_index_.compression_type();
    }
private:
    size_t offset_;
    std::string_view key_;
    DataBlockIndex data_block_index_;
    bool data_block_loaded_ = false;
};

struct EntryView {
    EntryView(std::string& k, std::string& v): key(k), value(v) {
        
    }
    EntryView(std::string_view k, std::string_view v): key(k), value(v) {
        
    }
    std::string_view key;
    std::string_view value;
};

class IndexBlockIndex {
public:
    size_t Load(char* s) {
        size_t index = 0;
        binary_size_ = *reinterpret_cast<size_t*>(s);
        index += sizeof(size_t);
        size_ = *reinterpret_cast<size_t*>(s + index);
        index += sizeof(size_t);
        data_block_indexs_.clear();
        data_block_indexs_.reserve(size_);
        // std::cout << " binary size " << binary_size_ << " size " << size_ << std::endl;
        for (size_t i = 0; i < size_; i++) {
            DataBlockIndexIndex data_block_index_index;
            index += data_block_index_index.Load(s + index, s);
            data_block_indexs_.emplace_back(std::move(data_block_index_index));
        }
        return index;
    }
    
    size_t data_block_size() {
        return data_block_indexs_.size();
    }

    bool Get(std::string_view key, std::string& value) {
        // 1.find data_block
        // 2.search data_block
        {
            size_t l = 0, r = data_block_indexs_.size();
            while (l < r) {
                size_t mid = (l + r) >> 1;
                if (data_block_indexs_[mid].key() > key) {
                    r = mid;
                } else {
                    l = mid + 1;
                }
            }
            if (r != 0) {
                return data_block_indexs_[r - 1].Get().Get(key, value);
            }
        }
        return false;
    }

    const std::string_view key() const {
        return data_block_indexs_.begin()->key();
    }

    std::vector<DataBlockIndexIndex>& data_block_index() {
        return data_block_indexs_;
    }

    bool HasCompressedBlock() const {
        for (const auto& block : data_block_indexs_) {
            if (block.IsCompressed()) {
                return true;
            }
        }
        return false;
    }

    common::CompressionType compression_type() const {
        for (const auto& block : data_block_indexs_) {
            if (block.IsCompressed()) {
                return block.compression_type();
            }
        }
        return common::CompressionType::kNone;
    }
private:
    size_t binary_size_;
    size_t size_{0};
    std::vector<DataBlockIndexIndex> data_block_indexs_;
};

/**
 * SST - Sorted String Table
 * 磁盘上的持久化有序键值对存储
 *
 * 使用 mmap 映射文件，避免内核态和用户态之间的数据拷贝
 * 内存管理由操作系统负责页面缓存
 */
class SST {
public:
    class Iterator {
    public:
        Iterator(SST* sst, bool rbegin = false): sst_(sst) {
            data_block_index_it_ = sst_->data_block_index().end();
            if (sst_->data_block_index().empty()) {
                return;
            }

            if (rbegin) {
                data_block_index_it_ = sst_->data_block_index().end();
                --data_block_index_it_;
                if (data_block_index_it_->Get().data_index().empty()) {
                    data_block_index_it_ = sst_->data_block_index().end();
                    return;
                }
                data_block_entry_it_ = data_block_index_it_->Get().data_index().end();
                --data_block_entry_it_;
            } else {
                data_block_index_it_ = sst_->data_block_index().begin();
                if (data_block_index_it_->Get().data_index().empty()) {
                    data_block_index_it_ = sst_->data_block_index().end();
                    return;
                }
                data_block_entry_it_ = data_block_index_it_->Get().data_index().begin();
            }
        }

        EntryIndex& operator * () {
            return *data_block_entry_it_;
        }

        bool operator ! () {
            return data_block_index_it_ == sst_->data_block_index().end();
        }

        explicit operator bool() {
            return data_block_index_it_ != sst_->data_block_index().end();
        }

        Iterator& operator ++ () {
            if (data_block_index_it_ == sst_->data_block_index().end()) {
                return *this;
            }

            ++data_block_entry_it_;
            if (data_block_entry_it_ == data_block_index_it_->Get().data_index().end()) {
                ++data_block_index_it_;
                if (data_block_index_it_ == sst_->data_block_index().end()) {
                    return *this;
                }
                data_block_entry_it_ = data_block_index_it_->Get().data_index().begin();
            }
            return *this;
        }
    private:
        std::vector<DataBlockIndexIndex>::iterator data_block_index_it_;
        std::vector<EntryIndex>::iterator data_block_entry_it_;
        SST* sst_;
    };

    SST() {
        compression_config_.enable = false;
        compression_config_.type = common::CompressionType::kNone;
    }

    SST(std::vector<EntryView> entries, int id) {
        compression_config_.enable = false;
        compression_config_.type = common::CompressionType::kNone;

        common::BloomFilter bloom_filter;
        bloom_filter.Init(entries.size(), 0.01);
        size_t data_block_size = sizeof(size_t); // data_block_size
        size_t index_block_size = 2 * sizeof(size_t) * (entries.size() + 1);
        index_block_size += (*entries.begin()).key.size();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            bloom_filter.Insert((*it).key.data(), (*it).key.size());
            data_block_size += (*it).key.size() + (*it).value.size();
        }
        data_block_size += entries.size() * 2 * sizeof(size_t);
        data_block_size += bloom_filter.binary_size();
        data_block_size += sizeof(size_t); // cnt

        file_size_ = index_block_size + data_block_size;
        
        id_ = id;
        name_ = std::to_string(id_) + ".sst";
        // std::cout << "open file " << std::endl;
        fd_ = open(name_.c_str(), O_RDWR);
        if (fd_ == -1) {
            fd_ = open(name_.c_str(), O_RDWR | O_CREAT, 0700);
        }

        detail::EnsureFileSizeOrThrow(fd_, file_size_, name_);

        // std::cout << " fd is " << fd_ << " size is " << file_size_ << std::endl;
        data_ = (char*)mmap(NULL, file_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        // std::cout << " create data " << std::endl;
        char* index_block_ptr = data_;
        char* data_block_ptr = data_ + index_block_size;
        size_t index_block_index = 0;
        size_t data_block_index = 0;
        // std::cout << "assign " << std::endl;
        // memcpy(data_, &index_block_size, 4);
        *reinterpret_cast<size_t*>(index_block_ptr) = index_block_size;
        // std::cout << " index block size " << index_block_size << std::endl;
        // std::cout << " data block size " << data_block_size << std::endl;
        // std::cout << " bloom filter size " << bloom_filter.binary_size() << std::endl;
        index_block_index += sizeof(size_t);
        // std::cout << "assign 2" << std::endl;
        *reinterpret_cast<size_t*>(index_block_ptr + index_block_index) = 1; // cnt
        index_block_ptr += sizeof(size_t);
        *reinterpret_cast<size_t*>(index_block_ptr + index_block_index) = index_block_size; // offset
        index_block_index += sizeof(size_t);
        *reinterpret_cast<size_t*>(index_block_ptr + index_block_index) = (*entries.begin()).key.size(); // key size
        index_block_index += sizeof(size_t);
        memcpy(index_block_ptr + index_block_index, (*entries.begin()).key.data(), (*entries.begin()).key.size());

        // std::cout << "memcpy index_block" << std::endl;
        // std::cout << "offset " << data_block_ptr - data_ << std::endl;
        *reinterpret_cast<size_t*>(data_block_ptr) = data_block_size;
        data_block_index += sizeof(size_t);
        data_block_index += bloom_filter.Save(data_block_ptr + data_block_index);
        *reinterpret_cast<size_t*>(data_block_ptr + data_block_index) = entries.size();
        data_block_index += sizeof(size_t);
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            *reinterpret_cast<size_t*>(data_block_ptr + data_block_index) = (*it).key.size();
            data_block_index += sizeof(size_t);
            *reinterpret_cast<size_t*>(data_block_ptr + data_block_index) = (*it).value.size();
            data_block_index += sizeof(size_t);
            memcpy(data_block_ptr + data_block_index, (*it).key.data(), (*it).key.size());
            data_block_index += (*it).key.size();
            memcpy(data_block_ptr + data_block_index, (*it).value.data(), (*it).value.size());
            data_block_index += (*it).value.size();
        }

        // for (int i = 0; i < file_size_; i++) {
        //     std::cout << int(data_[i]) << " ";
        // } std::cout << std::endl;

        // std::cout << "load index" << std::endl;

        index_block.Load(data_);

        // std::cout << " finish loaded " << std::endl;
        loaded_ = true;
        ready_ = true;
    }

    SST(MemeTable& memtable, size_t id) {
        compression_config_.enable = false;
        compression_config_.type = common::CompressionType::kNone;
        // std::cout << "make sst " << id << std::endl;
        // 每个 memtable 保证了小于 pagesize，切分留到 compaction 做
        common::BloomFilter bloom_filter;
        bloom_filter.Init(memtable.size(), 0.01);
        size_t data_block_size = sizeof(size_t); // data_block_size
        size_t index_block_size = 2 * sizeof(size_t) * (memtable.size() + 1);
        index_block_size += (*memtable.begin()).key.size();
        for (auto it = memtable.begin(); it != memtable.end(); ++it) {
            bloom_filter.Insert((*it).key.c_str(), (*it).key.size());
            data_block_size += (*it).key.size() + (*it).value.size();
        }
        data_block_size += memtable.size() * 2 * sizeof(size_t);
        data_block_size += bloom_filter.binary_size();
        data_block_size += sizeof(size_t); // cnt

        file_size_ = index_block_size + data_block_size;
        
        id_ = id;
        name_ = std::to_string(id_) + ".sst";
        // std::cout << "open file " << std::endl;
        fd_ = open(name_.c_str(), O_RDWR);
        if (fd_ == -1) {
            fd_ = open(name_.c_str(), O_RDWR | O_CREAT, 0700);
        }

        detail::EnsureFileSizeOrThrow(fd_, file_size_, name_);

        // std::cout << " fd is " << fd_ << " size is " << file_size_ << std::endl;
        data_ = (char*)mmap(NULL, file_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        // std::cout << " create data " << std::endl;
        char* index_block_ptr = data_;
        char* data_block_ptr = data_ + index_block_size;
        size_t index_block_index = 0;
        size_t data_block_index = 0;
        *reinterpret_cast<size_t*>(index_block_ptr) = index_block_size;
        // std::cout << " index block size " << index_block_size << std::endl;
        // std::cout << " data block size " << data_block_size << std::endl;
        // std::cout << " bloom filter size " << bloom_filter.binary_size() << std::endl;
        index_block_index += sizeof(size_t);
        *reinterpret_cast<size_t*>(index_block_ptr + index_block_index) = 1; // cnt
        index_block_ptr += sizeof(size_t);
        *reinterpret_cast<size_t*>(index_block_ptr + index_block_index) = index_block_size; // offset
        index_block_index += sizeof(size_t);
        *reinterpret_cast<size_t*>(index_block_ptr + index_block_index) = (*memtable.begin()).key.size(); // key size
        index_block_index += sizeof(size_t);
        memcpy(index_block_ptr + index_block_index, (*memtable.begin()).key.c_str(), (*memtable.begin()).key.size());

        // std::cout << "memcpy index_block" << std::endl;
        // std::cout << "offset " << data_block_ptr - data_ << std::endl;
        *reinterpret_cast<size_t*>(data_block_ptr) = data_block_size;
        data_block_index += sizeof(size_t);
        data_block_index += bloom_filter.Save(data_block_ptr + data_block_index);
        *reinterpret_cast<size_t*>(data_block_ptr + data_block_index) = memtable.size();
        data_block_index += sizeof(size_t);
        for (auto it = memtable.begin(); it != memtable.end(); ++it) {
            *reinterpret_cast<size_t*>(data_block_ptr + data_block_index) = (*it).key.size();
            data_block_index += sizeof(size_t);
            *reinterpret_cast<size_t*>(data_block_ptr + data_block_index) = (*it).value.size();
            data_block_index += sizeof(size_t);
            memcpy(data_block_ptr + data_block_index, (*it).key.c_str(), (*it).key.size());
            data_block_index += (*it).key.size();
            memcpy(data_block_ptr + data_block_index, (*it).value.c_str(), (*it).value.size());
            data_block_index += (*it).value.size();
        }

        // for (int i = 0; i < file_size_; i++) {
        //     std::cout << int(data_[i]) << " ";
        // } std::cout << std::endl;

        // std::cout << "load index" << std::endl;

        index_block.Load(data_);

        // std::cout << " finish loaded " << std::endl;
        loaded_ = true;
        ready_ = true;
    }

    /**
     * @brief 使用压缩创建 SST（从 EntryView 向量）
     * @param entries 键值对列表
     * @param id SST 文件 ID
     * @param config 压缩配置
     */
    SST(std::vector<EntryView> entries, int id, const CompressionConfig& config)
        : compression_config_(config) {
        CompressedBlockBuilder builder(config);

        // 构建压缩数据块
        for (auto& entry : entries) {
            builder.Add(entry.key, entry.value);
        }
        std::vector<char> compressed_data = builder.Build();

        // 计算文件大小
        // IndexBlock: [size(8B) | count(8B) | (offset(8B) + key_size(8B) + key)]
        size_t index_block_size = 2 * sizeof(size_t) * (entries.size() + 1);
        index_block_size += entries.begin()->key.size();

        // DataBlock: [block_size(8B) | encoded_block...]
        file_size_ = index_block_size + sizeof(size_t) + compressed_data.size();
        uncompressed_size_ = index_block_size + builder.uncompressed_block_size();
        compressed_ = builder.used_compression();
        compression_config_.enable = compressed_;
        if (!compressed_) {
            compression_config_.type = common::CompressionType::kNone;
        }

        id_ = id;
        name_ = std::to_string(id_) + ".sst";

        fd_ = open(name_.c_str(), O_RDWR);
        if (fd_ == -1) {
            fd_ = open(name_.c_str(), O_RDWR | O_CREAT, 0700);
        }

        detail::EnsureFileSizeOrThrow(fd_, file_size_, name_);

        data_ = (char*)mmap(NULL, file_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        char* index_block_ptr = data_;
        char* data_block_ptr = data_ + index_block_size;

        // 写入 IndexBlock
        size_t index_block_index = 0;
        *reinterpret_cast<size_t*>(index_block_ptr) = index_block_size;
        index_block_index += sizeof(size_t);
        *reinterpret_cast<size_t*>(index_block_ptr + index_block_index) = 1;  // count
        index_block_ptr += sizeof(size_t);
        *reinterpret_cast<size_t*>(index_block_ptr + index_block_index) = index_block_size;  // offset
        index_block_index += sizeof(size_t);
        *reinterpret_cast<size_t*>(index_block_ptr + index_block_index) = entries.begin()->key.size();  // key size
        index_block_index += sizeof(size_t);
        memcpy(index_block_ptr + index_block_index, entries.begin()->key.data(), entries.begin()->key.size());

        // 写入压缩的 DataBlock
        detail::WriteSize(data_block_ptr, sizeof(size_t) + compressed_data.size());
        memcpy(data_block_ptr + sizeof(size_t), compressed_data.data(), compressed_data.size());

        index_block.Load(data_);
        loaded_ = true;
        ready_ = true;
    }

    /**
     * @brief 使用压缩创建 SST（从 MemTable）
     * @param memtable 内存表
     * @param id SST 文件 ID
     * @param config 压缩配置
     */
    SST(MemeTable& memtable, size_t id, const CompressionConfig& config)
        : compression_config_(config) {
        CompressedBlockBuilder builder(config);

        // 构建压缩数据块
        for (auto it = memtable.begin(); it != memtable.end(); ++it) {
            builder.Add(std::string_view((*it).key), std::string_view((*it).value));
        }
        std::vector<char> compressed_data = builder.Build();

        // 计算文件大小
        size_t index_block_size = 2 * sizeof(size_t) * (memtable.size() + 1);
        index_block_size += (*memtable.begin()).key.size();

        file_size_ = index_block_size + sizeof(size_t) + compressed_data.size();
        uncompressed_size_ = index_block_size + builder.uncompressed_block_size();
        compressed_ = builder.used_compression();
        compression_config_.enable = compressed_;
        if (!compressed_) {
            compression_config_.type = common::CompressionType::kNone;
        }

        id_ = id;
        name_ = std::to_string(id_) + ".sst";

        fd_ = open(name_.c_str(), O_RDWR);
        if (fd_ == -1) {
            fd_ = open(name_.c_str(), O_RDWR | O_CREAT, 0700);
        }

        detail::EnsureFileSizeOrThrow(fd_, file_size_, name_);

        data_ = (char*)mmap(NULL, file_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        char* index_block_ptr = data_;
        char* data_block_ptr = data_ + index_block_size;

        // 写入 IndexBlock
        size_t index_block_index = 0;
        *reinterpret_cast<size_t*>(index_block_ptr) = index_block_size;
        index_block_index += sizeof(size_t);
        *reinterpret_cast<size_t*>(index_block_ptr + index_block_index) = 1;  // count
        index_block_ptr += sizeof(size_t);
        *reinterpret_cast<size_t*>(index_block_ptr + index_block_index) = index_block_size;  // offset
        index_block_index += sizeof(size_t);
        *reinterpret_cast<size_t*>(index_block_ptr + index_block_index) = (*memtable.begin()).key.size();  // key size
        index_block_index += sizeof(size_t);
        memcpy(index_block_ptr + index_block_index, (*memtable.begin()).key.c_str(), (*memtable.begin()).key.size());

        // 写入压缩的 DataBlock
        detail::WriteSize(data_block_ptr, sizeof(size_t) + compressed_data.size());
        memcpy(data_block_ptr + sizeof(size_t), compressed_data.data(), compressed_data.size());

        index_block.Load(data_);
        loaded_ = true;
        ready_ = true;
    }

    Iterator begin() {
        return Iterator(this);
    }

    Iterator rbegin() { // can NOT move
        return Iterator(this, true);
    }

    size_t id() const {
        return id_;
    }

    ~SST() {
        // std::cout << " ~SST " << std::endl;
        if (ready_) {
            Close();
        }
    }

    bool ready() {
        return ready_;
    }

    bool IsLoaded() {
        return loaded_;
    }

    size_t binary_size() {
        return file_size_;
    }

    void SetId(int id) {
        id_ = id;
        name_ = std::to_string(id_) + ".sst";
    }
    bool Load() {
        fd_ = open(name_.c_str(), O_RDWR);
        if (fd_ == -1) {
            return false;
        }
        struct stat stat_buf;
        if (fstat(fd_, &stat_buf) == -1) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        file_size_ = stat_buf.st_size;
        data_ = (char*)mmap(NULL, file_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (data_ == MAP_FAILED) {
            close(fd_);
            fd_ = -1;
            data_ = nullptr;
            return false;
        }
        index_block.Load(data_);
        compressed_ = index_block.HasCompressedBlock();
        compression_config_.enable = compressed_;
        compression_config_.type = index_block.compression_type();
        loaded_ = true;
        ready_ = true;
        return true;
    }

    void Close() {
        if (!loaded_) {
            return;
        }
        // std::cout << " in close " << std::endl;
        munmap(data_, file_size_);
        // std::cout << "finish munmap" << std::endl;
        if (fd_ != -1) {
            close(fd_);
        }
        ready_ = false;
        loaded_ = false;
    }

    const std::string_view key() const {
        return index_block.key();
    }

    bool Get(std::string_view key, std::string& value) {
        return index_block.Get(key, value);
    }

    std::vector<DataBlockIndexIndex>& data_block_index() {
        return index_block.data_block_index();
    }

    // ============ 压缩相关接口 ============

    /// @brief 设置压缩配置
    void SetCompressionConfig(const CompressionConfig& config) {
        compression_config_ = config;
    }

    /// @brief 获取压缩配置
    const CompressionConfig& GetCompressionConfig() const {
        return compression_config_;
    }

    /// @brief 检查是否使用压缩
    bool IsCompressed() const {
        return compressed_;
    }

    /// @brief 获取压缩率统计
    double GetCompressionRatio() const {
        if (file_size_ == 0 || uncompressed_size_ == 0) {
            return 1.0;
        }
        return static_cast<double>(file_size_) / uncompressed_size_;
    }

    // ============ 缓存相关接口 ============

    /// @brief 设置 Block Cache
    void SetBlockCache(BlockCache* cache) {
        block_cache_ = cache;
    }

    /// @brief 获取 Block Cache
    BlockCache* GetBlockCache() const {
        return block_cache_;
    }

    /// @brief 预加载 DataBlock 到缓存
    /// @param block_index 数据块索引
    /// @return true 预加载成功（无论是否已缓存），false 参数无效或无缓存
    bool PrefetchDataBlock(size_t block_index) {
        // 参数检查
        if (!block_cache_ || block_index >= data_block_index().size()) {
            return false;
        }

        // 获取 block 偏移量（从 IndexBlock 获取，而非 DataBlock）
        size_t block_offset = data_block_index()[block_index].offset();

        // 检查是否已缓存
        if (block_cache_->Get(id_, block_offset) != nullptr) {
            return true;  // 已缓存
        }

        // 从 mmap 内存读取 block 大小和数据
        // DataBlock 格式: [size(8B)][bloom_filter][count(8B)][entries...]
        size_t index = block_offset;
        size_t block_size = detail::ReadSize(data_ + index);
        index += sizeof(size_t);

        // 检查 block 大小是否有效
        if (block_size == 0 || block_offset + block_size > file_size_) {
            return false;
        }

        // 复制数据到缓存
        std::vector<char> block_data(data_ + index, data_ + block_offset + block_size);

        // 放入缓存
        return block_cache_->Put(id_, block_offset, std::move(block_data));
    }

    /// @brief 预加载多个 DataBlock 到缓存
    /// @param start_index 起始块索引
    /// @param count 预加载块数量
    /// @return 实际预加载的块数量
    size_t PrefetchDataBlocks(size_t start_index, size_t count) {
        size_t prefetched = 0;
        for (size_t i = 0; i < count; ++i) {
            if (PrefetchDataBlock(start_index + i)) {
                ++prefetched;
            }
        }
        return prefetched;
    }

    /// @brief 预热整个 SST 文件到缓存
    /// @return 预热的块数量
    size_t PrefetchAllBlocks() {
        return PrefetchDataBlocks(0, data_block_index().size());
    }

    /// @brief 获取缓存命中率
    double GetCacheHitRate() const {
        if (!block_cache_) return 0.0;
        return block_cache_->GetStats().HitRate();
    }

private:
    bool ready_ = false;
    int64_t id_ = 0;
    std::string name_;
    char* data_;
    int fd_ = -1;
    IndexBlockIndex index_block;
    bool loaded_ = false;
    size_t file_size_ = 0;
    CompressionConfig compression_config_;  ///< 压缩配置
    size_t uncompressed_size_{0};           ///< 未压缩时的大小
    BlockCache* block_cache_{nullptr};      ///< Block Cache（不拥有所有权）
    bool compressed_{false};                ///< 文件内是否包含压缩数据块
};


}
}
