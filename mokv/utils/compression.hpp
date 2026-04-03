/**
 * @file compression.hpp
 * @brief 数据压缩支持 - Snappy/LZ4 压缩算法封装
 *
 * 支持两种压缩算法：
 * - Snappy: Google 高速压缩算法，适合 x86 架构
 * - LZ4: 极高速压缩算法，压缩率与速度的平衡
 *
 * @section compression_benefits 压缩收益
 * - 减少磁盘 I/O：压缩后数据更小，读取更快
 * - 降低存储空间：典型压缩比 2-4x
 * - 提高缓存效率：更多数据可放入 Block Cache
 *
 * @section compression_format 压缩数据格式
 * - Snappy: 使用仓内兼容实现，原始大小由上层元数据保存
 * - LZ4: 使用标准 LZ4 Frame 格式，兼容 liblz4 / lz4 CLI
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <lz4frame.h>

namespace mokv {
namespace common {

/**
 * @brief 压缩类型枚举
 */
enum class CompressionType {
    kNone = 0,   ///< 无压缩
    kSnappy = 1, ///< Snappy 压缩
    kLZ4 = 2     ///< LZ4 压缩
};

/**
 * @brief 压缩结果类
 */
class CompressedData {
public:
    CompressedData() = default;

    CompressedData(std::vector<char>&& data, size_t original_size)
        : data_(std::move(data)), original_size_(original_size) {}

    CompressedData(const char* data, size_t size, size_t original_size)
        : data_(data, data + size), original_size_(original_size) {}

    /// @brief 获取压缩后的数据指针
    const char* data() const { return data_.data(); }

    /// @brief 获取压缩后的大小
    size_t size() const { return data_.size(); }

    /// @brief 获取原始数据大小
    size_t original_size() const { return original_size_; }

    /// @brief 检查是否为空
    bool empty() const { return data_.empty(); }

private:
    std::vector<char> data_;      ///< 压缩后的数据
    size_t original_size_{0};     ///< 原始数据大小
};

/**
 * @brief 压缩器基类
 */
class Compressor {
public:
    virtual ~Compressor() = default;

    /// @brief 获取压缩类型
    virtual CompressionType type() const = 0;

    /// @brief 压缩数据
    /// @param data 输入数据指针
    /// @param size 输入数据大小
    /// @return 压缩结果
    virtual CompressedData Compress(const char* data, size_t size) = 0;

    /// @brief 解压数据
    /// @param compressed_data 压缩数据
    /// @param output 输出缓冲区（需预先分配足够空间）
    /// @param output_size 输出缓冲区大小
    /// @return 实际解压大小
    virtual size_t Decompress(const CompressedData& compressed_data,
                              char* output, size_t output_size) = 0;

    /// @brief 获取解压后所需缓冲区大小
    virtual size_t GetDecompressedSize(const char* compressed_data, size_t compressed_size) = 0;

    /// @brief 计算压缩后的最大大小
    virtual size_t MaxCompressedSize(size_t original_size) = 0;
};

/**
 * @brief Snappy 压缩器
 *
 * Snappy 是 Google 开发的高速压缩算法：
 * - 压缩速度：数百 MB/s
 * - 解压速度：GB/s 级别
 * - 压缩比：通常 1.5-2x
 */
class SnappyCompressor : public Compressor {
public:
    CompressionType type() const override { return CompressionType::kSnappy; }

    CompressedData Compress(const char* data, size_t size) override {
        if (size == 0) {
            return CompressedData({}, 0);
        }

        // Snappy 压缩：输出大小最大为 32 + size * 1.0001
        size_t max_output = MaxCompressedSize(size);
        std::vector<char> output(max_output);

        size_t compressed_size = 0;
        snappy_compress(data, size, output.data(), &compressed_size);
        output.resize(compressed_size);

        return CompressedData(std::move(output), size);
    }

    size_t Decompress(const CompressedData& compressed_data,
                      char* output, size_t output_size) override {
        if (compressed_data.empty() || output_size < compressed_data.original_size()) {
            return 0;
        }
        snappy_uncompress(compressed_data.data(), compressed_data.size(), output);
        return compressed_data.original_size();
    }

    size_t GetDecompressedSize(const char* compressed_data, size_t compressed_size) override {
        size_t result = 0;
        snappy_uncompressed_length(compressed_data, compressed_size, &result);
        return result;
    }

    size_t MaxCompressedSize(size_t original_size) override {
        return snappy_max_compressed_length(original_size);
    }

private:
    // Snappy 嵌入式实现（避免外部依赖）
    static void snappy_compress(const char* input, size_t input_length,
                                 char* output, size_t* output_length) {
        *output_length = 0;
        size_t i = 0;
        while (i < input_length) {
            // 生成字面量或匹配
            if (i < input_length - 3 && input[i] == input[i+1] &&
                input[i] == input[i+2] && input[i] == input[i+3]) {
                // 重复字符
                size_t j = i + 4;
                while (j < input_length && input[j] == input[i]) ++j;
                size_t length = j - i;
                if (length < 12) {
                    // 字面量长度
                    output[(*output_length)++] = static_cast<char>(length - 4);
                    memcpy(output + *output_length, input + i, length);
                    *output_length += length;
                } else {
                    // 回溯引用
                    output[(*output_length)++] = 0xFC;
                    size_t emit = length;
                    while (emit > 0) {
                        size_t copylen = (emit > 64) ? 64 : emit;
                        output[(*output_length)++] = static_cast<char>(copylen - 4);
                        output[(*output_length)++] = static_cast<char>((i - 4) & 0xFF);
                        output[(*output_length)++] = static_cast<char>(((i - 4) >> 8) & 0xFF);
                        emit -= copylen;
                    }
                }
                i += length;
            } else {
                // 字面量
                size_t literal = 1;
                while (i + literal < input_length && literal < 60) {
                    if (i + literal < input_length - 3 &&
                        input[i + literal] == input[i + literal + 1] &&
                        input[i + literal] == input[i + literal + 2]) {
                        break;
                    }
                    ++literal;
                }
                output[(*output_length)++] = static_cast<char>(literal);
                memcpy(output + *output_length, input + i, literal);
                *output_length += literal;
                i += literal;
            }
        }
    }

    static void snappy_uncompress(const char* compressed, size_t compressed_length,
                                   char* decompressed) {
        size_t i = 0, j = 0;
        while (i < compressed_length) {
            uint8_t op = static_cast<uint8_t>(compressed[i++]);
            if (op < 64) {
                // 字面量
                memcpy(decompressed + j, compressed + i, op + 1);
                i += op + 1;
                j += op + 1;
            } else if (op < 252) {
                // 回溯引用
                size_t length = op - 60;
                size_t byte1 = static_cast<size_t>(compressed[i++]) & 0xFF;
                size_t byte2 = static_cast<size_t>(compressed[i++]) & 0xFF;
                size_t offset = byte1 | (byte2 << 8);
                for (size_t k = 0; k < length; ++k) {
                    decompressed[j + k] = decompressed[j + k - offset - 4];
                }
                j += length;
            } else if (op == 252) {
                // 4字节回溯
                size_t length = static_cast<size_t>(compressed[i++]);
                size_t byte1 = static_cast<size_t>(compressed[i++]) & 0xFF;
                size_t byte2 = static_cast<size_t>(compressed[i++]) & 0xFF;
                size_t byte3 = static_cast<size_t>(compressed[i++]) & 0xFF;
                size_t offset = byte1 | (byte2 << 8) | (byte3 << 16);
                for (size_t k = 0; k < length; ++k) {
                    decompressed[j + k] = decompressed[j + k - offset - 4];
                }
                j += length;
            } else if (op == 253) {
                // 保留
                i += 3;
            } else if (op == 254 || op == 255) {
                // 保留
                i += 2;
            }
        }
    }

    static void snappy_uncompressed_length(const char* compressed, size_t compressed_length,
                                            size_t* result) {
        // 简化实现：返回保守估计
        *result = compressed_length;
    }

    static size_t snappy_max_compressed_length(size_t input_length) {
        return 32 + input_length + (input_length / 6);
    }
};

/**
 * @brief LZ4 压缩器
 *
 * LZ4 是极高速的压缩算法：
 * - 压缩速度：极快（GB/s）
 * - 解压速度：更快
 * - 压缩比：通常 2-3x
 *
 * 适用于对延迟敏感的场景。
 */
class LZ4Compressor : public Compressor {
public:
    CompressionType type() const override { return CompressionType::kLZ4; }

    CompressedData Compress(const char* data, size_t size) override {
        if (size == 0) {
            return CompressedData({}, 0);
        }

        LZ4F_preferences_t preferences = MakePreferences(size);
        const size_t max_output = LZ4F_compressFrameBound(size, &preferences);
        std::vector<char> output(max_output);

        const size_t compressed_size = LZ4F_compressFrame(
            output.data(), output.size(), data, size, &preferences);
        if (LZ4F_isError(compressed_size)) {
            return CompressedData({}, 0);
        }
        output.resize(compressed_size);

        return CompressedData(std::move(output), size);
    }

    size_t Decompress(const CompressedData& compressed_data,
                      char* output, size_t output_size) override {
        if (compressed_data.empty()) {
            return 0;
        }

        if (output_size < compressed_data.original_size()) {
            return 0;
        }

        LZ4F_dctx* raw_ctx = nullptr;
        const auto create_result = LZ4F_createDecompressionContext(&raw_ctx, LZ4F_VERSION);
        if (LZ4F_isError(create_result) || raw_ctx == nullptr) {
            return 0;
        }

        std::unique_ptr<LZ4F_dctx, DctxDeleter> ctx(raw_ctx);
        const char* src = compressed_data.data();
        size_t src_remaining = compressed_data.size();
        char* dst = output;
        size_t dst_remaining = output_size;
        bool finished = false;

        while (src_remaining > 0 && dst_remaining > 0) {
            size_t src_size = src_remaining;
            size_t dst_size = dst_remaining;
            const size_t result = LZ4F_decompress(
                ctx.get(), dst, &dst_size, src, &src_size, nullptr);
            if (LZ4F_isError(result)) {
                return 0;
            }
            if (src_size == 0 && dst_size == 0) {
                return 0;
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

        if (!finished) {
            return 0;
        }

        const size_t decompressed_size = output_size - dst_remaining;
        if (decompressed_size != compressed_data.original_size()) {
            return 0;
        }
        return decompressed_size;
    }

    size_t GetDecompressedSize(const char* compressed_data, size_t compressed_size) override {
        if (compressed_data == nullptr || compressed_size == 0) {
            return 0;
        }

        LZ4F_dctx* raw_ctx = nullptr;
        const auto create_result = LZ4F_createDecompressionContext(&raw_ctx, LZ4F_VERSION);
        if (LZ4F_isError(create_result) || raw_ctx == nullptr) {
            return 0;
        }

        std::unique_ptr<LZ4F_dctx, DctxDeleter> ctx(raw_ctx);
        LZ4F_frameInfo_t frame_info{};
        size_t src_size = compressed_size;
        const size_t result = LZ4F_getFrameInfo(ctx.get(), &frame_info, compressed_data, &src_size);
        if (LZ4F_isError(result) || frame_info.contentSize == 0 ||
            frame_info.contentSize > std::numeric_limits<size_t>::max()) {
            return 0;
        }
        return static_cast<size_t>(frame_info.contentSize);
    }

    size_t MaxCompressedSize(size_t original_size) override {
        LZ4F_preferences_t preferences = MakePreferences(original_size);
        return LZ4F_compressFrameBound(original_size, &preferences);
    }

private:
    struct DctxDeleter {
        void operator()(LZ4F_dctx* ctx) const {
            if (ctx != nullptr) {
                LZ4F_freeDecompressionContext(ctx);
            }
        }
    };

    static LZ4F_preferences_t MakePreferences(size_t size) {
        LZ4F_preferences_t preferences = LZ4F_INIT_PREFERENCES;
        preferences.frameInfo.blockMode = LZ4F_blockIndependent;
        preferences.frameInfo.contentSize = static_cast<unsigned long long>(size);
        return preferences;
    }
};

/**
 * @brief 压缩工厂类
 *
 * 提供统一的压缩器创建接口，支持运行时选择压缩算法。
 */
class CompressionFactory {
public:
    /// @brief 获取指定类型的压缩器
    static std::unique_ptr<Compressor> Create(CompressionType type) {
        switch (type) {
            case CompressionType::kSnappy:
                return std::make_unique<SnappyCompressor>();
            case CompressionType::kLZ4:
                return std::make_unique<LZ4Compressor>();
            case CompressionType::kNone:
            default:
                return nullptr;
        }
    }

    /// @brief 根据压缩率选择最优算法
    static CompressionType SelectByRatio(size_t original_size, size_t compressed_size) {
        if (original_size == 0) return CompressionType::kNone;

        double ratio = static_cast<double>(original_size) / compressed_size;
        // 压缩比 > 1.5x 使用压缩，否则不使用
        return ratio > 1.5 ? CompressionType::kLZ4 : CompressionType::kNone;
    }

    /// @brief 根据数据特征自动选择算法
    static CompressionType AutoSelect(const char* data, size_t size) {
        // LZ4 通常在速度和压缩率之间有更好的平衡
        return CompressionType::kLZ4;
    }
};

/**
 * @brief 压缩 Block 辅助类
 *
 * 用于管理 DataBlock 的压缩/解压，
 * 提供透明的数据压缩访问接口。
 */
class CompressedBlock {
public:
    CompressedBlock() = default;

    /// @brief 使用原始数据初始化（不压缩）
    void Init(const char* data, size_t size) {
        data_.assign(data, data + size);
        compressed_ = false;
        original_size_ = size;
        compression_type_ = CompressionType::kNone;
    }

    /// @brief 使用指定压缩算法压缩数据
    void Compress(CompressionType type) {
        if (data_.empty()) return;

        auto compressor = CompressionFactory::Create(type);
        if (!compressor) return;

        CompressedData compressed = compressor->Compress(data_.data(), data_.size());
        if (!compressed.empty()) {
            data_.assign(compressed.data(), compressed.data() + compressed.size());
            compressed_ = true;
            original_size_ = compressed.original_size();
            compression_type_ = type;
        }
    }

    /// @brief 解压数据
    void Decompress() {
        if (!compressed_ || data_.empty()) return;

        auto compressor = CompressionFactory::Create(compression_type_);
        if (!compressor) return;

        CompressedData compressed(data_.data(), data_.size(), original_size_);
        std::vector<char> decompressed(original_size_);
        size_t result = compressor->Decompress(compressed, decompressed.data(), original_size_);
        if (result > 0) {
            data_ = std::move(decompressed);
            compressed_ = false;
            compression_type_ = CompressionType::kNone;
        }
    }

    /// @brief 获取数据指针
    const char* data() const { return data_.data(); }

    /// @brief 获取数据大小
    size_t size() const { return data_.size(); }

    /// @brief 获取原始大小
    size_t original_size() const { return original_size_; }

    /// @brief 是否已压缩
    bool compressed() const { return compressed_; }

    /// @brief 交换数据
    void Swap(std::vector<char>& other) {
        data_.swap(other);
    }

private:
    std::vector<char> data_;      ///< 存储的数据（可能已压缩）
    size_t original_size_{0};     ///< 原始数据大小
    bool compressed_{false};      ///< 是否已压缩
    CompressionType compression_type_{CompressionType::kNone};
};

}  // namespace common
}  // namespace mokv
